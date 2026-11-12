#include "VikunjaStore.h"

#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>
#include <VikunjaConfig.h>
#include <WiFi.h>

#include <optional>
#include <string>

#include "dev/DevApi.h"
#include "network/HttpDownloader.h"
#include "network/WifiJoin.h"

namespace vikunja {
namespace {

constexpr const char* kDir = "/.crosspoint/vikunja";
constexpr const char* kProjectsPath = "/.crosspoint/vikunja/projects.json";
constexpr const char* kPendingPath = "/.crosspoint/vikunja-pending.json";
constexpr size_t kMaxSecretChars = 1024;
constexpr size_t kMaxConfigBytes = 16 * 1024;
constexpr size_t kMaxSmallFileBytes = 256 * 1024;
// A saved task list: about 30 bytes a line, so this is far past any list.
constexpr size_t kMaxTasksFileBytes = 4 * 1024 * 1024;
// One page of full task objects is about 60 KB; cap a misbehaving server.
constexpr size_t kMaxPageBytes = 1024 * 1024;
// Below this much contiguous PSRAM, stop loading and keep what we have.
constexpr size_t kMinPsramBlock = 512 * 1024;
constexpr unsigned long kWifiJoinMs = 10000;

bool isEnabled = false;
Config cfg;
std::string token;
std::vector<HttpDownloader::Header> headers;
std::vector<Project> savedProjects;
PendingTicks waiting;
Status status = Status::Ok;
bool wifiTurnedOn = false;
// A failed join is not retried until the user asks (Sync row) or leaves
// Lists: each attempt blocks the screen for kWifiJoinMs.
bool wifiJoinFailed = false;
// One visit to Lists (until it is left): whether the full sync has been
// tried, and which projects' items have been fetched. Each is tried once a
// visit; the Sync row asks again.
bool visitSynced = false;
// Items per page the server really sends: a page that was followed by a
// non-empty one shows its cap when that is below kPageSize.
size_t serverPageSize = kPageSize;

void notePageSize(const size_t count, const size_t nextCount) {
  if (count > 0 && nextCount > 0 && count < serverPageSize) serverPageSize = count;
}
std::vector<int64_t> fetchedProjects;

void markFetched(const int64_t projectId) {
  for (const int64_t id : fetchedProjects) {
    if (id == projectId) return;
  }
  fetchedProjects.push_back(projectId);
}

bool writeAtomically(const char* path, const std::string& content) {
  const std::string tmp = std::string(path) + ".tmp";
  return Storage.writeFile(tmp.c_str(), String(content.c_str())) && Storage.replaceFile(tmp.c_str(), path);
}

std::string tasksPath(const int64_t projectId) { return std::string(kDir) + "/p" + std::to_string(projectId) + ".txt"; }

// Decodes a stored secret; empty when it does not decode on this chip.
std::string decode(const std::string& obf) {
  bool ok = false, tooLong = false;
  std::string plain = obfuscation::deobfuscateFromBase64(obf.c_str(), kMaxSecretChars, &ok, &tooLong);
  if (!ok || tooLong) plain.clear();
  return plain;
}

// Moves plain secrets into their _obf fields and rewrites the config. On a
// failed write the plain values stay in the file and are tried next boot.
void storeSecretsObfuscated() {
  bool changed = false;
  if (!cfg.token.empty()) {
    cfg.tokenObf = obfuscation::obfuscateToBase64(cfg.token).c_str();
    changed = true;
  }
  for (auto& h : cfg.headers) {
    if (h.value.empty()) continue;
    h.valueObf = obfuscation::obfuscateToBase64(h.value).c_str();
    changed = true;
  }
  if (!changed) return;
  if (!writeAtomically(kConfigPath, serializeConfig(cfg))) {
    LOG_ERR("VIK", "Vikunja config: could not rewrite %s", kConfigPath);
    return;
  }
  LOG_INF("VIK", "Vikunja config: secrets stored obfuscated");
}

bool lowOnPsram() {
#ifdef SIMULATOR
  return false;
#else
  return HalMemory::getPsramHeap().largestBlockBytes < kMinPsramBlock;
#endif
}

bool wifiUp() {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (wifiJoinFailed) return false;
  const bool wasOff = WiFi.getMode() == WIFI_MODE_NULL;
  if (!joinSavedWifi(kWifiJoinMs)) {
    wifiJoinFailed = true;
    // Don't leave the radio searching for the rest of the visit (it would
    // be in the store); a connection, once made, stays up for the visit.
    if (wasOff && !dev_api::enabled()) {
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      LOG_INF("VIK", "Wi-Fi join failed; radio off");
    }
    return false;
  }
  wifiTurnedOn = true;  // this join made the connection; endVisit drops it
  return true;
}

HttpDownloader::FetchOptions requestOptions(int* httpStatus) {
  HttpDownloader::FetchOptions options;
  options.bearer = token;
  options.headers = headers;
  options.accept = "application/json";
  options.statusOut = httpStatus;
  return options;
}

// GET an API path into body. *httpStatus gets the raw status.
Status get(const std::string& path, std::string& body, int* httpStatus = nullptr) {
  body.clear();
  body.reserve(64 * 1024);  // over the 4 KB threshold, so in PSRAM
  bool tooBig = false;
  int code = 0;
  HttpDownloader::fetchUrl(
      cfg.url + "/api/v1" + path,
      [&body, &tooBig](const uint8_t* data, const size_t len) {
        if (body.size() + len > kMaxPageBytes) {
          tooBig = true;
          return false;
        }
        body.append(reinterpret_cast<const char*>(data), len);
        return true;
      },
      requestOptions(&code));
  if (httpStatus) *httpStatus = code;
  if (tooBig) {
    LOG_ERR("VIK", "response over %u bytes: %s", static_cast<unsigned>(kMaxPageBytes), path.c_str());
    return Status::ServerDown;
  }
  const Status s = classifyResponse(code, HttpDownloader::lastAnswerRedirected());
  if (s != Status::Ok) LOG_ERR("VIK", "GET %s: HTTP %d", path.c_str(), code);
  return s;
}

// Sends one waiting tick. Ok also covers a tick dropped because the task is
// gone or already in the wanted state.
Status sendTick(const Tick& tick) {
  const std::string path = "/tasks/" + std::to_string(tick.taskId);
  std::string body;
  int code = 0;
  const Status s = get(path, body, &code);
  // Only a 404 from the server itself (not a redirect) means deleted; sync
  // has already had a good answer from it this run.
  if (code == 404 && s == Status::ServerDown) {
    LOG_INF("VIK", "task %lld deleted on the server; tick dropped", static_cast<long long>(tick.taskId));
    return Status::Ok;
  }
  if (s != Status::Ok) return s;
  TaskList one;
  std::optional<bool> serverDone;
  if (appendTasksPage("[" + body + "]", one) && one.size() == 1) serverDone = one.done(0);
  if (!serverDone) return Status::ServerDown;
  if (resolveTick(tick, serverDone) == TickAction::Drop) return Status::Ok;
  if (!setTopLevelDone(body, tick.done)) return Status::ServerDown;
  int postCode = 0;
  HttpDownloader::postJson(cfg.url + "/api/v1" + path, body, requestOptions(&postCode));
  const Status posted = classifyResponse(postCode, false);
  if (posted == Status::Ok) return posted;
  LOG_ERR("VIK", "POST %s: HTTP %d", path.c_str(), postCode);
  // The same credentials just read the task, so a 4xx here (a read-only
  // share, a rejected body) will never succeed: drop this tick rather than
  // block every tick queued after it. 401, 5xx and no answer are retried.
  if (postCode >= 400 && postCode < 500 && postCode != 401) {
    LOG_ERR("VIK", "task %lld refused the change; tick dropped", static_cast<long long>(tick.taskId));
    return Status::Ok;
  }
  return posted;
}

// Writes sent ticks into their projects' saved copies, so the items stay
// ticked when the list is next opened offline.
void applyToSavedCopies(const std::vector<Tick>& sent) {
  std::vector<int64_t> done;
  for (const auto& tick : sent) {
    if (tick.projectId == 0) continue;
    bool seen = false;
    for (const int64_t id : done) seen = seen || id == tick.projectId;
    if (seen) continue;
    done.push_back(tick.projectId);
    TaskList saved;
    if (!loadTasks(tick.projectId, saved)) continue;
    bool changed = false;
    for (const auto& t : sent) {
      if (t.projectId == tick.projectId) changed = applySent(saved, t.taskId, t.done) || changed;
    }
    if (changed) writeAtomically(tasksPath(tick.projectId).c_str(), serializeTasks(saved));
  }
}

Status sendTicks() {
  const std::vector<Tick> ticks = waiting.all();
  std::vector<Tick> sent;
  Status s = Status::Ok;
  for (const auto& tick : ticks) {
    s = sendTick(tick);
    if (s != Status::Ok) break;
    waiting.remove(tick.taskId);
    savePending();
    sent.push_back(tick);
  }
  applyToSavedCopies(sent);
  return s;
}

Status fetchProjects(std::vector<Project>& out) {
  std::string body;
  size_t previousCount = 0;
  for (int pageNo = 1;; ++pageNo) {
    if (lowOnPsram()) return Status::Incomplete;
    const Status s = get("/projects?per_page=" + std::to_string(kPageSize) + "&page=" + std::to_string(pageNo), body);
    if (s != Status::Ok) return s;
    std::vector<Project> page;
    if (!appendProjectsPage(body, page)) return Status::ServerDown;
    notePageSize(previousCount, page.size());
    previousCount = page.size();
    // A server may cap per_page lower than asked, so a short page does not
    // end the list; an empty one, or one with nothing new (a server
    // ignoring `page`), does.
    size_t added = 0;
    for (auto& p : page) {
      if (findProject(out, p.id)) continue;
      out.push_back(std::move(p));
      ++added;
    }
    if (added == 0) return Status::Ok;
  }
}

// Appends whole pages of a view's tasks, skipping ids already in `out`.
// maxPages 0: until a page adds nothing (empty, or a server ignoring
// `page`). *more: whether another page may follow.
Status fetchTasks(const std::string& query, TaskList& out, const int firstPage, const int maxPages, bool* more) {
  std::string body;
  TaskList page;
  size_t previousCount = 0;
  if (more) *more = false;
  for (int n = 0; maxPages == 0 || n < maxPages; ++n) {
    if (lowOnPsram()) return Status::Incomplete;
    const char* sep = query.back() == '?' ? "" : "&";
    const Status s =
        get(query + sep + "per_page=" + std::to_string(kPageSize) + "&page=" + std::to_string(firstPage + n), body);
    if (s != Status::Ok) return s;
    page.clear();
    size_t count = 0;
    if (!appendTasksPage(body, page, &count)) return Status::ServerDown;
    notePageSize(previousCount, count);
    previousCount = count;
    size_t added = 0;
    for (size_t i = 0; i < page.size(); ++i) {
      if (out.find(page.id(i)) >= 0) continue;
      out.add(page.id(i), page.title(i), page.done(i));
      ++added;
    }
    if (count == 0 || added == 0) return Status::Ok;
  }
  if (more) *more = true;
  return Status::Ok;
}

std::string viewQuery(const Project& p, const int64_t viewId) {
  return "/projects/" + std::to_string(p.id) + "/views/" + std::to_string(viewId) + "/tasks?";
}

std::string doneQuery(const Project& p) {
  return viewQuery(p, p.tableViewId) + "filter=done%20%3D%20true&sort_by=done_at&order_by=desc";
}

Status fetchProjectTasks(const Project& p, TaskList& out) {
  out.clear();
  if (p.listViewId == 0) return Status::ServerDown;
  Status s = fetchTasks(viewQuery(p, p.listViewId), out, 1, 0, nullptr);
  if (s != Status::Ok || p.tableViewId == 0) return s;
  return fetchTasks(doneQuery(p), out, 1, 1, nullptr);
}

Status finish(const Status s) {
  status = s;
  return s;
}

}  // namespace

void loadStore() {
  isEnabled = false;
#if defined(BOARD_HAS_PSRAM) || defined(SIMULATOR)
  if (!Storage.exists(kConfigPath)) return;
  std::string json;
  if (!Storage.readFileToString("VIK", kConfigPath, kMaxConfigBytes, json)) {
    LOG_ERR("VIK", "Vikunja config: could not read %s", kConfigPath);
    return;
  }
  std::string error;
  if (!parseConfig(json, cfg, &error)) {
    LOG_ERR("VIK", "Vikunja config: %s", error.c_str());
    return;
  }
  token = cfg.token;
  std::vector<std::string> values;
  for (const auto& h : cfg.headers) values.push_back(h.value);
  storeSecretsObfuscated();
  if (token.empty()) token = decode(cfg.tokenObf);
  if (token.empty()) {
    LOG_ERR("VIK", "Vikunja config: stored token does not decode on this device; add \"token\" again");
    return;
  }
  headers.clear();
  for (size_t i = 0; i < cfg.headers.size(); ++i) {
    std::string value = values[i].empty() ? decode(cfg.headers[i].valueObf) : values[i];
    if (value.empty()) {
      LOG_ERR("VIK", "Vikunja config: header %s does not decode on this device; add \"value\" again",
              cfg.headers[i].name.c_str());
      return;
    }
    headers.emplace_back(cfg.headers[i].name, std::move(value));
  }
  cfg.token.clear();
  for (auto& h : cfg.headers) h.value.clear();

  Storage.ensureDirectoryExists(kDir);
  std::string saved;
  if (Storage.exists(kProjectsPath) && Storage.readFileToString("VIK", kProjectsPath, kMaxSmallFileBytes, saved) &&
      !parseProjects(saved, savedProjects)) {
    LOG_ERR("VIK", "saved projects unreadable; ignored");
  }
  if (Storage.exists(kPendingPath) && Storage.readFileToString("VIK", kPendingPath, kMaxSmallFileBytes, saved) &&
      !waiting.parse(saved)) {
    LOG_ERR("VIK", "waiting ticks unreadable; ignored");
  }
  isEnabled = true;
  LOG_INF("VIK", "Vikunja: enabled, %u saved projects, %u waiting ticks", static_cast<unsigned>(savedProjects.size()),
          static_cast<unsigned>(waiting.all().size()));
#endif
}

bool enabled() { return isEnabled; }

const std::vector<Project>& projects() { return savedProjects; }

bool loadTasks(const int64_t projectId, TaskList& out) {
  const std::string path = tasksPath(projectId);
  std::string text;
  if (!Storage.exists(path.c_str()) || !Storage.readFileToString("VIK", path, kMaxTasksFileBytes, text)) return false;
  return parseTasks(text, out);
}

PendingTicks& pending() { return waiting; }

bool savePending() {
  if (writeAtomically(kPendingPath, waiting.serialize())) return true;
  LOG_ERR("VIK", "could not save waiting ticks");
  return false;
}

namespace {

// Fetches one project's items into *tasks and saves them.
Status fetchAndSave(const int64_t projectId, TaskList* tasks) {
  const Project* p = findProject(savedProjects, projectId);
  if (!p) return Status::Ok;  // deleted on the server; the screen goes back
  TaskList loaded;
  const Status s = fetchProjectTasks(*p, loaded);
  if (s != Status::Ok && s != Status::Incomplete) return s;
  *tasks = std::move(loaded);
  if (s == Status::Ok && !writeAtomically(tasksPath(projectId).c_str(), serializeTasks(*tasks))) {
    LOG_ERR("VIK", "could not save tasks of project %lld", static_cast<long long>(projectId));
  }
  return s;
}

}  // namespace

Status sync(const int64_t projectId, TaskList* tasks) {
  visitSynced = true;
  fetchedProjects.clear();
  if (projectId != 0) markFetched(projectId);
  if (!wifiUp()) return finish(Status::Offline);
  // Projects first: ticks are sent only once the server has answered as
  // Vikunja, so a stray 404 elsewhere cannot drop one.
  std::vector<Project> fresh;
  Status s = fetchProjects(fresh);
  if (s != Status::Ok) return finish(s);
  savedProjects = std::move(fresh);
  if (!writeAtomically(kProjectsPath, serializeProjects(savedProjects))) LOG_ERR("VIK", "could not save projects");

  s = sendTicks();
  if (s != Status::Ok) return finish(s);

  if (projectId == 0 || !tasks) return finish(Status::Ok);
  return finish(fetchAndSave(projectId, tasks));
}

Status refreshProject(const int64_t projectId, TaskList* tasks) {
  markFetched(projectId);
  if (!wifiUp()) return finish(Status::Offline);
  return finish(fetchAndSave(projectId, tasks));
}

bool syncedThisVisit() { return visitSynced; }

size_t pageSize() { return serverPageSize; }

bool fetchedThisVisit(const int64_t projectId) {
  for (const int64_t id : fetchedProjects) {
    if (id == projectId) return true;
  }
  return false;
}

Status loadMoreDone(const int64_t projectId, int* page, TaskList& tasks, bool* more) {
  *more = false;
  const Project* p = findProject(savedProjects, projectId);
  if (!p || p->tableViewId == 0) return finish(Status::ServerDown);
  if (!wifiUp()) return finish(Status::Offline);
  // Pages already loaded (after Back and reopening Completed) add nothing;
  // skip past them to the first page with something new.
  std::string body;
  TaskList fetched;
  for (int skipped = 0; skipped < 1000; ++skipped, ++*page) {
    if (lowOnPsram()) return finish(Status::Incomplete);
    const Status s =
        get(doneQuery(*p) + "&per_page=" + std::to_string(kPageSize) + "&page=" + std::to_string(*page), body);
    if (s != Status::Ok) return finish(s);
    fetched.clear();
    size_t count = 0;
    if (!appendTasksPage(body, fetched, &count)) return finish(Status::ServerDown);
    if (count == 0) return finish(Status::Ok);
    size_t added = 0;
    for (size_t i = 0; i < fetched.size(); ++i) {
      if (tasks.find(fetched.id(i)) >= 0) continue;
      tasks.add(fetched.id(i), fetched.title(i), fetched.done(i));
      ++added;
    }
    if (added > 0) {
      ++*page;
      *more = true;
      writeAtomically(tasksPath(projectId).c_str(), serializeTasks(tasks));
      return finish(Status::Ok);
    }
  }
  return finish(Status::Ok);
}

Status lastStatus() { return status; }

void retryWifi() { wifiJoinFailed = false; }

void endVisit() {
  wifiJoinFailed = false;
  visitSynced = false;
  fetchedProjects.clear();
  if (!wifiTurnedOn || dev_api::enabled()) return;
  wifiTurnedOn = false;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  LOG_INF("VIK", "Wi-Fi off");
}

}  // namespace vikunja
