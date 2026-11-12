#include "VikunjaModel.h"

#include <ArduinoJson.h>

#include <cstdlib>

namespace vikunja {
namespace {

// Parses a JSON array (or null) into doc, keeping only what `filter` names.
// True for a body that is an array or the literal null. Checked on the text
// because a filtered parse turns any other value into null.
bool isArrayText(std::string_view json) {
  const size_t first = json.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) return false;
  if (json[first] == '[') return true;
  const size_t last = json.find_last_not_of(" \t\r\n");
  return json.substr(first, last - first + 1) == "null";
}

bool parseArray(std::string_view json, JsonDocument& doc, const JsonDocument& filter) {
  if (!isArrayText(json)) return false;
  return deserializeJson(doc, json.data(), json.size(), DeserializationOption::Filter(filter)) ==
         DeserializationError::Ok;
}

bool parseArray(std::string_view json, JsonDocument& doc) {
  if (!isArrayText(json)) return false;
  return deserializeJson(doc, json.data(), json.size()) == DeserializationError::Ok;
}

Project projectFromApi(JsonObjectConst item) {
  Project p;
  p.id = item["id"] | int64_t{0};
  p.title = item["title"] | "";
  p.parentId = item["parent_project_id"] | int64_t{0};
  p.archived = item["is_archived"] | false;
  p.starred = item["is_favorite"] | false;
  for (JsonObjectConst view : item["views"].as<JsonArrayConst>()) {
    const std::string_view kind = view["view_kind"] | "";
    const int64_t id = view["id"] | int64_t{0};
    if (kind == "list" && p.listViewId == 0) p.listViewId = id;
    if (kind == "table" && p.tableViewId == 0) p.tableViewId = id;
  }
  return p;
}

void appendEscaped(std::string& out, std::string_view title) {
  for (const char c : title) {
    if (c == '\\') {
      out += "\\\\";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c != '\r' && c != '\0') {  // a NUL would end the file early
      out += c;
    }
  }
}

void unescapeInto(std::string& out, std::string_view escaped) {
  out.clear();
  for (size_t i = 0; i < escaped.size(); ++i) {
    char c = escaped[i];
    if (c == '\\' && i + 1 < escaped.size()) {
      const char next = escaped[++i];
      c = next == 't' ? '\t' : next == 'n' ? '\n' : next;
    }
    out += c;
  }
}

bool parseId(std::string_view field, int64_t& id) {
  if (field.empty() || field.size() > 19) return false;
  id = 0;
  for (const char c : field) {
    if (c < '0' || c > '9') return false;
    id = id * 10 + (c - '0');
  }
  return id > 0;
}

}  // namespace

void TaskList::clear() {
  rows.clear();
  text.clear();
}

void TaskList::add(const int64_t id, std::string_view title, const bool done) {
  rows.push_back({id, static_cast<uint32_t>(text.size()), static_cast<uint32_t>(title.size()), done});
  text.append(title);
}

int TaskList::find(const int64_t id) const {
  for (size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

bool appendProjectsPage(std::string_view json, std::vector<Project>& out) {
  JsonDocument filter;
  filter[0]["id"] = true;
  filter[0]["title"] = true;
  filter[0]["parent_project_id"] = true;
  filter[0]["is_archived"] = true;
  filter[0]["is_favorite"] = true;
  filter[0]["views"][0]["id"] = true;
  filter[0]["views"][0]["view_kind"] = true;
  JsonDocument doc;
  if (!parseArray(json, doc, filter)) return false;
  for (JsonObjectConst item : doc.as<JsonArrayConst>()) {
    Project p = projectFromApi(item);
    if (p.id > 0) out.push_back(std::move(p));
  }
  return true;
}

bool appendTasksPage(std::string_view json, TaskList& out, size_t* count) {
  JsonDocument filter;
  filter[0]["id"] = true;
  filter[0]["title"] = true;
  filter[0]["done"] = true;
  JsonDocument doc;
  if (!parseArray(json, doc, filter)) return false;
  const JsonArrayConst items = doc.as<JsonArrayConst>();
  if (count) *count = items.size();
  for (JsonObjectConst item : items) {
    const int64_t id = item["id"] | int64_t{0};
    if (id > 0) out.add(id, item["title"] | "", item["done"] | false);
  }
  return true;
}

std::string serializeProjects(const std::vector<Project>& projects) {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (const auto& p : projects) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = p.id;
    o["title"] = p.title;
    o["parent"] = p.parentId;
    o["list_view"] = p.listViewId;
    o["table_view"] = p.tableViewId;
    o["archived"] = p.archived;
    o["starred"] = p.starred;
  }
  std::string out;
  serializeJson(doc, out);
  return out;
}

bool parseProjects(std::string_view json, std::vector<Project>& out) {
  JsonDocument doc;
  if (!parseArray(json, doc)) return false;
  out.clear();
  for (JsonObjectConst o : doc.as<JsonArrayConst>()) {
    Project p;
    p.id = o["id"] | int64_t{0};
    p.title = o["title"] | "";
    p.parentId = o["parent"] | int64_t{0};
    p.listViewId = o["list_view"] | int64_t{0};
    p.tableViewId = o["table_view"] | int64_t{0};
    p.archived = o["archived"] | false;
    p.starred = o["starred"] | false;
    out.push_back(std::move(p));
  }
  return true;
}

std::string serializeTasks(const TaskList& tasks) {
  std::string out;
  for (size_t i = 0; i < tasks.size(); ++i) {
    out += std::to_string(tasks.id(i));
    out += tasks.done(i) ? "\t1\t" : "\t0\t";
    appendEscaped(out, tasks.title(i));
    out += '\n';
  }
  return out;
}

bool parseTasks(std::string_view text, TaskList& out) {
  TaskList parsed;
  std::string title;
  while (!text.empty()) {
    const size_t end = text.find('\n');
    const std::string_view line = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    if (line.empty()) continue;
    const size_t tab1 = line.find('\t');
    if (tab1 == std::string_view::npos || line.size() < tab1 + 3 || line[tab1 + 2] != '\t') return false;
    int64_t id = 0;
    if (!parseId(line.substr(0, tab1), id)) return false;
    const char doneFlag = line[tab1 + 1];
    if (doneFlag != '0' && doneFlag != '1') return false;
    unescapeInto(title, line.substr(tab1 + 3));
    parsed.add(id, title, doneFlag == '1');
  }
  out = std::move(parsed);
  return true;
}

const Project* findProject(const std::vector<Project>& projects, const int64_t id) {
  for (const auto& p : projects) {
    if (p.id == id) return &p;
  }
  return nullptr;
}

int64_t favouriteProject(const std::vector<Project>& projects, const int64_t chosen) {
  if (chosen != 0 && findProject(projects, chosen)) return chosen;
  for (const auto& p : projects) {
    if (p.starred && !p.archived) return p.id;
  }
  return 0;
}

bool setTopLevelDone(std::string& json, const bool done) {
  int depth = 0;
  size_t i = 0;
  const size_t n = json.size();
  while (i < n) {
    const char c = json[i];
    if (c == '"') {
      const size_t start = i + 1;
      ++i;
      while (i < n && json[i] != '"') i += json[i] == '\\' ? 2 : 1;
      if (i >= n) return false;
      const std::string_view str(json.data() + start, i - start);
      ++i;
      if (depth != 1 || str != "done") continue;
      // A key at depth 1 is followed by ':'; a string value is not.
      size_t j = i;
      while (j < n && (json[j] == ' ' || json[j] == '\t' || json[j] == '\n' || json[j] == '\r')) ++j;
      if (j >= n || json[j] != ':') continue;
      ++j;
      while (j < n && (json[j] == ' ' || json[j] == '\t' || json[j] == '\n' || json[j] == '\r')) ++j;
      const size_t len = json.compare(j, 4, "true") == 0 ? 4 : json.compare(j, 5, "false") == 0 ? 5 : 0;
      if (len == 0) return false;
      json.replace(j, len, done ? "true" : "false");
      return true;
    }
    if (c == '{' || c == '[') ++depth;
    if (c == '}' || c == ']') --depth;
    ++i;
  }
  return false;
}

}  // namespace vikunja
