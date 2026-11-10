#pragma once

#include <functional>
#include <string>
#include <vector>

// Pure planning for mirroring one OPDS feed (a Calibre-Web shelf) to a folder
// on the SD card. No I/O: callers pass the remote listing, the manifest of
// files the mirror created, and a file-existence predicate.
//
// Nothing is ever deleted. A book that leaves the current shelf (removed, or a
// different shelf was chosen) is hidden: moved into a dot-folder the library
// skips. When it comes back it is moved to its original path, so per-book
// caches and reading position keyed by that path still apply.
namespace shelf_mirror {

// One downloadable book seen in the remote feed.
struct RemoteBook {
  std::string id;  // stable per book (Calibre UUID); falls back to href when empty
  std::string title;
  std::string author;
  std::string href;
};

// One file the mirror downloaded. `path` is where the book lives while
// visible; a hidden book sits at hiddenPath(path).
struct Record {
  std::string id;
  std::string path;
  bool hidden = false;
};

struct Plan {
  std::vector<RemoteBook> download;  // on the shelf, never downloaded (or its file is gone)
  std::vector<Record> unhide;        // on the shelf, currently hidden
  std::vector<Record> hide;          // visible, no longer on the shelf
  std::vector<Record> keep;          // unchanged
};

using ExistsFn = std::function<bool(const std::string& path)>;

// Folder that hidden books move into, inside the visible folder.
constexpr const char* kHiddenDirName = ".cache";

// "/Calibre/A.epub" -> "/Calibre/.cache/A.epub"
std::string hiddenPath(const std::string& path);

// Where the record's file currently is.
inline std::string currentPath(const Record& rec) { return rec.hidden ? hiddenPath(rec.path) : rec.path; }

// Diff the remote shelf against the manifest.
// walkComplete must be true only when every feed page was fetched and parsed
// without truncation. Books are hidden only after a complete walk that found
// at least one book: a partial or empty listing (Calibre-Web answers a deleted
// shelf with an empty feed) must never hide the library.
// A record whose file is gone is dropped; if the book is on the shelf it is
// downloaded again.
Plan plan(const std::vector<RemoteBook>& remote, const std::vector<Record>& manifest, bool walkComplete,
          const ExistsFn& fileExists);

// Manifest text format: a "shelf-mirror v1" header, then one
// "id<TAB>v|h<TAB>path" line per record. Malformed lines are skipped. Returns
// false when the text is not a manifest (wrong header); empty text is a valid,
// empty manifest.
std::string serializeManifest(const std::vector<Record>& records);
bool parseManifest(const std::string& text, std::vector<Record>& out);

// Returns `folder/filename`, or `folder/stem (2).ext`, `(3)`, ... when the
// path is taken. `filename` must already be sanitized.
std::string uniquePath(const std::string& folder, const std::string& filename, const ExistsFn& taken);

}  // namespace shelf_mirror
