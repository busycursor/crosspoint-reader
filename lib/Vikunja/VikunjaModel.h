#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Vikunja projects and tasks: API pages in, the SD saved copy in and out.
namespace vikunja {

struct Project {
  int64_t id = 0;
  std::string title;
  int64_t parentId = 0;     // 0: top level
  int64_t listViewId = 0;   // open items, in the Vikunja list's order
  int64_t tableViewId = 0;  // no filter of its own; used for done items
  bool archived = false;
  bool starred = false;  // Vikunja's favourite star
  bool operator==(const Project&) const = default;
};

// One project's tasks, in the order the server sent them: open items in the
// Vikunja list's order, then done items most recently done first. Titles
// share one buffer, so a long list is two allocations (both in PSRAM once
// past the 4 KB malloc threshold), not one per title on the internal heap.
class TaskList {
 public:
  void clear();
  void add(int64_t id, std::string_view title, bool done);
  size_t size() const { return rows.size(); }
  int64_t id(size_t i) const { return rows[i].id; }
  std::string_view title(size_t i) const { return std::string_view(text).substr(rows[i].offset, rows[i].length); }
  bool done(size_t i) const { return rows[i].done; }
  void setDone(size_t i, bool done) { rows[i].done = done; }
  // Index of the task, or -1.
  int find(int64_t id) const;
  bool operator==(const TaskList&) const = default;

 private:
  struct Row {
    int64_t id;
    uint32_t offset;
    uint32_t length;
    bool done;
    bool operator==(const Row&) const = default;
  };
  std::vector<Row> rows;
  std::string text;
};

// Append one API page (a JSON array; null counts as empty). False on
// malformed JSON or a non-array body. *count gets the page's task count.
bool appendProjectsPage(std::string_view json, std::vector<Project>& out);
bool appendTasksPage(std::string_view json, TaskList& out, size_t* count = nullptr);

std::string serializeProjects(const std::vector<Project>& projects);
bool parseProjects(std::string_view json, std::vector<Project>& out);

// Saved copy of a TaskList: one "id<TAB>0|1<TAB>title" line per task, with
// \\, \t and \n escaped in the title (\r and NUL dropped). Read without
// building a JSON tree.
std::string serializeTasks(const TaskList& tasks);
bool parseTasks(std::string_view text, TaskList& out);

const Project* findProject(const std::vector<Project>& projects, int64_t id);
// The list Lists opens into: `chosen` (the device setting) when set and
// present, else the first starred, non-archived project; 0 for none.
int64_t favouriteProject(const std::vector<Project>& projects, int64_t chosen);

// Sets the top-level "done" member of a task object in place, leaving every
// other byte as the server sent it. False when there is none.
bool setTopLevelDone(std::string& taskJson, bool done);

}  // namespace vikunja
