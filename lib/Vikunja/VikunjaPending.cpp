#include "VikunjaPending.h"

#include <ArduinoJson.h>

#include <algorithm>

namespace vikunja {
namespace {

const Tick* findTick(const std::vector<Tick>& ticks, const int64_t taskId) {
  for (const auto& t : ticks) {
    if (t.taskId == taskId) return &t;
  }
  return nullptr;
}

}  // namespace

void PendingTicks::toggle(const int64_t taskId, const bool done, const bool serverDone, const int64_t projectId) {
  remove(taskId);
  if (done != serverDone) ticks.push_back({taskId, done, projectId});
}

void PendingTicks::remove(const int64_t taskId) {
  ticks.erase(std::remove_if(ticks.begin(), ticks.end(), [taskId](const Tick& t) { return t.taskId == taskId; }),
              ticks.end());
}

std::string PendingTicks::serialize() const {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (const auto& t : ticks) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = t.taskId;
    o["done"] = t.done;
    if (t.projectId != 0) o["project"] = t.projectId;
  }
  std::string out;
  serializeJson(doc, out);
  return out;
}

bool PendingTicks::parse(std::string_view json) {
  JsonDocument doc;
  if (deserializeJson(doc, json.data(), json.size()) != DeserializationError::Ok || !doc.is<JsonArray>()) {
    return false;
  }
  std::vector<Tick> parsed;
  for (JsonObjectConst o : doc.as<JsonArrayConst>()) {
    const int64_t id = o["id"] | int64_t{0};
    if (id > 0) parsed.push_back({id, o["done"] | false, o["project"] | int64_t{0}});
  }
  ticks = std::move(parsed);
  return true;
}

void splitTasks(const TaskList& saved, const PendingTicks& pending, std::vector<size_t>& open,
                std::vector<size_t>& done) {
  open.clear();
  done.clear();
  open.reserve(saved.size());
  const auto& ticks = pending.all();
  // Ticked items, newest tick first.
  for (auto it = ticks.rbegin(); it != ticks.rend(); ++it) {
    const int i = saved.find(it->taskId);
    if (it->done && i >= 0 && !saved.done(i)) done.push_back(i);
  }
  for (size_t i = 0; i < saved.size(); ++i) {
    // Placed by the tick loops. A tick the server already matches (seen after
    // a reload, before sync drops it) shows the item where it is.
    const Tick* tick = findTick(ticks, saved.id(i));
    if (tick && tick->done != saved.done(i)) continue;
    (saved.done(i) ? done : open).push_back(i);
  }
  // Unticked items go to the end of the open list, oldest untick first.
  for (const auto& tick : ticks) {
    const int i = saved.find(tick.taskId);
    if (!tick.done && i >= 0 && saved.done(i)) open.push_back(i);
  }
}

bool applySent(TaskList& saved, const int64_t taskId, const bool done) {
  const int i = saved.find(taskId);
  if (i < 0) return false;
  saved.setDone(static_cast<size_t>(i), done);
  return true;
}

TickAction resolveTick(const Tick& tick, const std::optional<bool> serverDone) {
  if (!serverDone || *serverDone == tick.done) return TickAction::Drop;
  return TickAction::Send;
}

}  // namespace vikunja
