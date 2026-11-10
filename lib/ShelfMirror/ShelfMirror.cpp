#include "ShelfMirror.h"

#include <unordered_set>

namespace shelf_mirror {
namespace {

constexpr const char* kHeader = "shelf-mirror v1";

const std::string& keyOf(const RemoteBook& book) { return book.id.empty() ? book.href : book.id; }

bool hasLineBreakOrTab(const std::string& s) { return s.find_first_of("\t\r\n") != std::string::npos; }

}  // namespace

std::string hiddenPath(const std::string& path) {
  const size_t slash = path.rfind('/');
  const std::string dir = slash == std::string::npos ? "" : path.substr(0, slash);
  const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  return dir + "/" + kHiddenDirName + "/" + name;
}

Plan plan(const std::vector<RemoteBook>& remote, const std::vector<Record>& manifest, const bool walkComplete,
          const ExistsFn& fileExists) {
  Plan out;

  // Feed pages can shift while they are walked, so one book may appear twice.
  std::unordered_set<std::string> remoteIds;
  std::vector<const RemoteBook*> remoteUnique;
  remoteUnique.reserve(remote.size());
  for (const auto& book : remote) {
    const std::string& key = keyOf(book);
    if (key.empty()) continue;
    if (remoteIds.insert(key).second) remoteUnique.push_back(&book);
  }
  const bool mayHide = walkComplete && !remoteUnique.empty();

  std::unordered_set<std::string> onShelfAndHeld;  // ids already covered by a record
  for (const auto& rec : manifest) {
    if (!fileExists(currentPath(rec))) continue;  // gone: forget it

    const bool onShelf = remoteIds.count(rec.id) != 0;
    // A second record for the same book is surplus; park it out of sight.
    const bool surplus = onShelf && onShelfAndHeld.count(rec.id) != 0;

    if (onShelf && !surplus) {
      onShelfAndHeld.insert(rec.id);
      (rec.hidden ? out.unhide : out.keep).push_back(rec);
    } else if (!rec.hidden && (surplus || mayHide)) {
      out.hide.push_back(rec);
    } else {
      out.keep.push_back(rec);
    }
  }

  for (const RemoteBook* book : remoteUnique) {
    if (onShelfAndHeld.count(keyOf(*book)) == 0) out.download.push_back(*book);
  }
  return out;
}

std::string serializeManifest(const std::vector<Record>& records) {
  std::string text = kHeader;
  text += '\n';
  for (const auto& rec : records) {
    if (rec.id.empty() || rec.path.empty() || hasLineBreakOrTab(rec.id) || hasLineBreakOrTab(rec.path)) continue;
    text += rec.id;
    text += rec.hidden ? "\th\t" : "\tv\t";
    text += rec.path;
    text += '\n';
  }
  return text;
}

bool parseManifest(const std::string& text, std::vector<Record>& records) {
  records.clear();
  if (text.empty()) return true;
  size_t pos = 0;
  bool first = true;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(pos, end - pos);
    pos = end + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (first) {
      first = false;
      if (line != kHeader) return false;
      continue;
    }
    const size_t tab1 = line.find('\t');
    if (tab1 == std::string::npos || tab1 == 0) continue;
    const size_t tab2 = line.find('\t', tab1 + 1);
    if (tab2 != tab1 + 2 || tab2 + 1 >= line.size()) continue;
    const char state = line[tab1 + 1];
    if (state != 'v' && state != 'h') continue;
    records.push_back({line.substr(0, tab1), line.substr(tab2 + 1), state == 'h'});
  }
  return true;
}

std::string uniquePath(const std::string& folder, const std::string& filename, const ExistsFn& taken) {
  std::string base = folder;
  if (base.empty() || base.back() != '/') base += '/';
  std::string candidate = base + filename;
  if (!taken(candidate)) return candidate;

  const size_t dot = filename.rfind('.');
  const std::string stem = (dot == std::string::npos || dot == 0) ? filename : filename.substr(0, dot);
  const std::string ext = (dot == std::string::npos || dot == 0) ? "" : filename.substr(dot);
  for (int n = 2; n < 1000; ++n) {
    candidate = base + stem + " (" + std::to_string(n) + ")" + ext;
    if (!taken(candidate)) return candidate;
  }
  return candidate;
}

}  // namespace shelf_mirror
