#include "ShelfSyncActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <OpdsFeedParser.h>

#include <unordered_set>
#include <utility>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "util/BookCacheUtils.h"
#include "util/OpdsFilename.h"
#include "util/UrlUtils.h"

namespace fui = freeink::ui;

namespace {

constexpr const char* kConfigPath = "/.crosspoint/shelf_mirror.cfg";
constexpr const char* kManifestPath = "/.crosspoint/shelf_mirror.txt";
constexpr const char* kManifestTmpPath = "/.crosspoint/shelf_mirror.txt.tmp";
constexpr const char* kFolder = "/Calibre";
// Calibre-Web serves its shelf list here; other OPDS servers fall back to the root feed.
constexpr const char* kShelfIndexPath = "/opds/shelfindex";
constexpr size_t kMaxSmallFile = 64 * 1024;
constexpr size_t kMaxManifestFile = 512 * 1024;
constexpr int kMaxPages = 200;

bool isEpub(const std::string& path) {
  uint8_t magic[4] = {0};
  HalFile f;
  if (Storage.openFileForRead("SYNC", path.c_str(), f)) f.read(magic, sizeof(magic));
  if (f.isOpen()) f.close();
  return magic[0] == 'P' && magic[1] == 'K' && magic[2] == 3 && magic[3] == 4;
}

}  // namespace

ShelfSyncActivity::ShelfSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, OpdsServer server)
    : CatalogActivity("ShelfSync", renderer, mappedInput), server(std::move(server)) {}

void ShelfSyncActivity::onEnter() {
  CatalogActivity::onEnter();
  opdsClient.setServer(server.url, server.username, server.password, server.activeCustomHeaders());
  opdsClient.resetAuthState();

  Config saved;
  pickingShelf = !(loadConfig(saved) && saved.serverUrl == server.url && !saved.feedUrl.empty());
  if (!pickingShelf) config = std::move(saved);

  state = State::CHECK_WIFI;
  statusMessage = tr(STR_CHECKING_WIFI);
  checkAndConnectWifi();
}

void ShelfSyncActivity::onExit() {
  closeRouting();
  rows.clear();
  rowItems.clear();
  CatalogActivity::onExit();
}

void ShelfSyncActivity::startBrowse() {
  if (pickingShelf)
    showShelfPicker();
  else
    runSync();
}

void ShelfSyncActivity::setRows(std::vector<Row> newRows) {
  RenderLock lock;
  closeRouting();
  nav.reset();
  rows = std::move(newRows);
  rowItems.clear();
  rowItems.reserve(rows.size());
  for (const auto& row : rows) {
    fui::ListItem item;
    item.label = row.label.c_str();
    if (!row.value.empty()) item.value = row.value.c_str();
    item.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(item);
  }
  state = State::BROWSING;
}

void ShelfSyncActivity::showShelfPicker() {
  beginLoading();
  std::string url = UrlUtils::buildUrl(server.url, kShelfIndexPath);
  OpdsFeedParser parser;
  auto status = opdsClient.fetchFeed(url, parser);
  if (status != freeink::opds::OpdsClient::FetchStatus::Ok) {
    // Not Calibre-Web: let the user pick any feed from the server root.
    url = server.url;
    parser.reset();
    status = opdsClient.fetchFeed(url, parser);
  }
  if (status != freeink::opds::OpdsClient::FetchStatus::Ok) {
    fail(status == freeink::opds::OpdsClient::FetchStatus::AuthFailed ? StrId::STR_OPDS_AUTH_FAILED
                                                                      : StrId::STR_FETCH_FEED_FAILED);
    return;
  }

  std::vector<Row> shelves;
  for (auto& entry : parser.takeEntries()) {
    if (entry.type != OpdsEntryType::NAVIGATION || entry.href.empty()) continue;
    shelves.push_back({std::move(entry.title), ">", UrlUtils::buildUrl(url, entry.href), ROW_SHELF});
  }
  if (shelves.empty()) {
    fail(StrId::STR_SHELF_SYNC_NO_SHELVES);
    return;
  }
  setRows(std::move(shelves));
  requestUpdate(true);
}

bool ShelfSyncActivity::walkShelf(std::vector<shelf_mirror::RemoteBook>& out, bool& complete) {
  complete = false;
  std::unordered_set<std::string> visited;
  std::string url = config.feedUrl;
  for (int page = 0; page < kMaxPages && !url.empty(); ++page) {
    if (!visited.insert(url).second) return true;  // pagination loop: stop, but don't trust it as complete
    OpdsFeedParser parser;
    const auto status = opdsClient.fetchFeed(url, parser);
    if (status != freeink::opds::OpdsClient::FetchStatus::Ok) {
      LOG_ERR("SYNC", "Shelf page %d failed: %d", page, static_cast<int>(status));
      return page > 0;  // first page failing means we know nothing
    }
    const bool truncated = parser.truncated();
    for (auto& entry : parser.takeEntries()) {
      if (entry.type != OpdsEntryType::BOOK || entry.lcpLicense || entry.purchase || entry.indirect) continue;
      out.push_back(
          {std::move(entry.id), std::move(entry.title), std::move(entry.author), UrlUtils::buildUrl(url, entry.href)});
    }
    if (truncated) return true;
    const std::string& next = parser.getNextPageUrl();
    if (next.empty()) {
      complete = true;
      return true;
    }
    url = UrlUtils::buildUrl(url, next);
  }
  return true;
}

void ShelfSyncActivity::runSync() {
  added = restored = hiddenCount = failed = 0;
  cancelled = incomplete = false;
  beginLoading();

  std::vector<shelf_mirror::RemoteBook> remote;
  bool complete = false;
  if (!walkShelf(remote, complete)) {
    syncFailed(StrId::STR_FETCH_FEED_FAILED);
    return;
  }
  if (complete && remote.empty()) {
    // Calibre-Web answers a deleted or hidden shelf with an empty feed.
    syncFailed(StrId::STR_SHELF_SYNC_EMPTY);
    return;
  }
  incomplete = !complete;

  std::vector<shelf_mirror::Record> manifest;
  if (!loadManifest(manifest)) {
    // Treating an unreadable record as empty would re-download every book
    // next to the old copies.
    syncFailed(StrId::STR_SHELF_SYNC_MANIFEST_ERROR);
    return;
  }
  const auto exists = [](const std::string& p) { return Storage.exists(p.c_str()); };
  shelf_mirror::Plan plan = shelf_mirror::plan(remote, manifest, complete, exists);
  std::vector<shelf_mirror::RemoteBook>().swap(remote);
  std::vector<shelf_mirror::Record>().swap(manifest);
  LOG_INF("SYNC", "Plan: %u download, %u unhide, %u hide, %u keep", static_cast<unsigned>(plan.download.size()),
          static_cast<unsigned>(plan.unhide.size()), static_cast<unsigned>(plan.hide.size()),
          static_cast<unsigned>(plan.keep.size()));

  if (!Storage.exists(kFolder)) Storage.mkdir(kFolder);
  std::vector<shelf_mirror::Record> records = std::move(plan.keep);
  // Visible paths owned by a record stay reserved even while the book is hidden.
  const auto pathTaken = [&records](const std::string& p) {
    for (const auto& r : records) {
      if (r.path == p) return true;
    }
    return Storage.exists(p.c_str());
  };

  // Hidden books keep their book cache (layout, reading position): it is keyed
  // by the visible path, which they return to.
  if (!plan.hide.empty()) {
    const std::string hiddenDir = std::string(kFolder) + "/" + shelf_mirror::kHiddenDirName;
    if (!Storage.exists(hiddenDir.c_str())) Storage.mkdir(hiddenDir.c_str());
  }
  for (auto& rec : plan.hide) {
    const std::string to = shelf_mirror::hiddenPath(rec.path);
    if (Storage.exists(to.c_str())) Storage.remove(to.c_str());  // stale copy in our own cache folder
    if (Storage.rename(rec.path.c_str(), to.c_str())) {
      rec.hidden = true;
      ++hiddenCount;
    } else {
      LOG_ERR("SYNC", "Could not hide %s", rec.path.c_str());
    }
    records.push_back(rec);
    if (!saveManifest(records)) {
      syncFailed(StrId::STR_SHELF_SYNC_MANIFEST_ERROR);
      return;
    }
  }

  for (auto& rec : plan.unhide) {
    const std::string from = shelf_mirror::hiddenPath(rec.path);
    std::string to = rec.path;
    if (Storage.exists(to.c_str())) {
      // Something else took the name meanwhile; the book's old cache no longer applies.
      const size_t slash = to.rfind('/');
      to = shelf_mirror::uniquePath(kFolder, to.substr(slash + 1), pathTaken);
    }
    if (Storage.rename(from.c_str(), to.c_str())) {
      if (to != rec.path) clearBookCache(to);
      rec.path = to;
      rec.hidden = false;
      ++restored;
    } else {
      LOG_ERR("SYNC", "Could not restore %s", from.c_str());
    }
    records.push_back(rec);
    if (!saveManifest(records)) {
      syncFailed(StrId::STR_SHELF_SYNC_MANIFEST_ERROR);
      return;
    }
  }

  const freeink::opds::HttpAuth auth = opdsClient.downloadAuth();
  std::vector<HttpDownloader::Header> headers = auth.headers;
  if (!auth.bearer.empty()) headers.push_back({"Authorization", "Bearer " + auth.bearer});

  const size_t total = plan.download.size();
  for (size_t i = 0; i < total; ++i) {
    const auto& book = plan.download[i];
    const std::string dest = shelf_mirror::uniquePath(
        kFolder,
        opdsBookFilename(book.author, book.title, static_cast<OpdsFilenameFormat>(SETTINGS.opdsFilenameFormat)),
        pathTaken);
    const std::string tmp = dest + ".part";

    char title[160];
    snprintf(title, sizeof(title), "(%u/%u) %s", static_cast<unsigned>(i + 1), static_cast<unsigned>(total),
             book.title.c_str());
    beginDownload(title);
    const auto result = downloadFile(book.href, tmp, auth.username, auth.password, headers);

    if (result == HttpDownloader::ABORTED) {
      if (Storage.exists(tmp.c_str())) Storage.remove(tmp.c_str());
      cancelled = true;
      break;
    }
    if (result != HttpDownloader::OK || !isEpub(tmp) || !Storage.rename(tmp.c_str(), dest.c_str())) {
      LOG_ERR("SYNC", "Download failed for %s (%d)", book.href.c_str(), static_cast<int>(result));
      if (Storage.exists(tmp.c_str())) Storage.remove(tmp.c_str());
      ++failed;
      continue;
    }
    clearBookCache(dest);  // a fresh file at this path; any cache for it is stale
    records.push_back({book.id.empty() ? book.href : book.id, dest, false});
    ++added;
    // After each book, so a power loss never leaves an untracked file.
    if (!saveManifest(records)) {
      syncFailed(StrId::STR_SHELF_SYNC_MANIFEST_ERROR);
      return;
    }
  }

  if (added > 0 || restored > 0 || hiddenCount > 0) library::markLibraryIndexDirty();
  showSummary();
}

void ShelfSyncActivity::syncFailed(const StrId message) {
  LOG_INF("SYNC", "RESULT ok=0 error=\"%s\"", I18N.get(message));
  fail(message);
}

void ShelfSyncActivity::showSummary() {
  LOG_INF("SYNC", "RESULT ok=1 added=%d restored=%d hidden=%d failed=%d cancelled=%d partial=%d", added, restored,
          hiddenCount, failed, cancelled ? 1 : 0, incomplete ? 1 : 0);
  char line[96];
  snprintf(line, sizeof(line), tr(STR_SHELF_SYNC_RESULT), added, restored, hiddenCount);
  std::vector<Row> summary;
  summary.push_back({line, "", "", ROW_SYNC_AGAIN});
  if (failed > 0) {
    char f[64];
    snprintf(f, sizeof(f), tr(STR_SHELF_SYNC_FAILED_COUNT), failed);
    summary.push_back({f, "", "", ROW_SYNC_AGAIN});
  }
  if (cancelled) summary.push_back({tr(STR_SHELF_SYNC_CANCELLED), "", "", ROW_SYNC_AGAIN});
  if (incomplete) summary.push_back({tr(STR_SHELF_SYNC_PARTIAL), "", "", ROW_SYNC_AGAIN});
  summary.push_back({tr(STR_SHELF_SYNC_AGAIN), "", "", ROW_SYNC_AGAIN});
  summary.push_back({tr(STR_SHELF_SYNC_CHOOSE), config.title, "", ROW_CHOOSE_SHELF});
  setRows(std::move(summary));
  requestUpdate(true);
}

void ShelfSyncActivity::activateIndex(const int index) {
  if (index < 0 || index >= static_cast<int>(rows.size())) return;
  app.clearTapFlash();
  const Row row = rows[index];
  switch (row.action) {
    case ROW_SHELF:
      config = {server.url, row.href, row.label};
      saveConfig(config);
      pickingShelf = false;
      runSync();
      break;
    case ROW_SYNC_AGAIN:
      runSync();
      break;
    case ROW_CHOOSE_SHELF:
      pickingShelf = true;
      showShelfPicker();
      break;
  }
}

void ShelfSyncActivity::buildScreen(UiScreen& screen) {
  const char* title = pickingShelf || config.title.empty() ? tr(STR_SHELF_SYNC) : config.title.c_str();
  screenHeader(screen, title);
  if (buildStatusScreen(screen, /*boldError=*/false, /*showDownloadTotal=*/true)) return;

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  syncListViewport(screen, props);
  screen.list(props);
}

void ShelfSyncActivity::drawFooter() {
  MappedInputManager::Labels labels;
  switch (state) {
    case State::BROWSING:
      labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      break;
    case State::DOWNLOADING:
      labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
      break;
    case State::ERROR:
      labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
      break;
    default:
      labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      break;
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

bool ShelfSyncActivity::loadConfig(Config& out) {
  std::string text;
  if (!Storage.exists(kConfigPath) || !Storage.readFileToString("SYNC", kConfigPath, kMaxSmallFile, text)) return false;
  std::string* fields[] = {&out.serverUrl, &out.feedUrl, &out.title};
  size_t pos = 0;
  for (auto* field : fields) {
    const size_t end = text.find('\n', pos);
    *field = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    if (end == std::string::npos) break;
    pos = end + 1;
  }
  return !out.serverUrl.empty() && !out.feedUrl.empty();
}

bool ShelfSyncActivity::saveConfig(const Config& cfg) {
  const std::string text = cfg.serverUrl + '\n' + cfg.feedUrl + '\n' + cfg.title + '\n';
  return Storage.writeFile(kConfigPath, String(text.c_str()));
}

// Missing manifest = first sync. A manifest that exists but cannot be read or
// parsed is an error, never an empty record.
bool ShelfSyncActivity::loadManifest(std::vector<shelf_mirror::Record>& out) {
  out.clear();
  // A power loss between remove and rename in saveManifest() leaves only the temp copy.
  const char* path = Storage.exists(kManifestPath)      ? kManifestPath
                     : Storage.exists(kManifestTmpPath) ? kManifestTmpPath
                                                        : nullptr;
  if (!path) return true;
  std::string text;
  if (!Storage.readFileToString("SYNC", path, kMaxManifestFile, text) || !shelf_mirror::parseManifest(text, out)) {
    LOG_ERR("SYNC", "Unreadable manifest %s", path);
    return false;
  }
  return true;
}

// Write to a temp file, then swap it in, so the manifest on disk is always whole.
bool ShelfSyncActivity::saveManifest(const std::vector<shelf_mirror::Record>& records) {
  const std::string text = shelf_mirror::serializeManifest(records);
  if (!Storage.writeFile(kManifestTmpPath, String(text.c_str()))) {
    LOG_ERR("SYNC", "Could not write manifest");
    return false;
  }
  if (Storage.exists(kManifestPath) && !Storage.remove(kManifestPath)) {
    LOG_ERR("SYNC", "Could not replace manifest");
    return false;
  }
  if (!Storage.rename(kManifestTmpPath, kManifestPath)) {
    LOG_ERR("SYNC", "Could not rename manifest");
    return false;
  }
  return true;
}
