#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "VikunjaModel.h"

// Ticks made on the device that the server has not seen yet.
namespace vikunja {

struct Tick {
  int64_t taskId = 0;
  bool done = false;      // the state the user wants
  int64_t projectId = 0;  // whose saved copy to update once sent
};

class PendingTicks {
 public:
  // Records that the user set taskId to `done`. When that matches the saved
  // server state, any waiting tick for it is dropped instead. A changed
  // tick moves to the end (the most recent).
  void toggle(int64_t taskId, bool done, bool serverDone, int64_t projectId = 0);
  void remove(int64_t taskId);
  const std::vector<Tick>& all() const { return ticks; }
  std::string serialize() const;
  // False (and no change) on malformed JSON.
  bool parse(std::string_view json);

 private:
  std::vector<Tick> ticks;  // oldest first
};

// The saved tasks with ticks applied. Open keeps the saved order, with
// unticked items at the end. Done has the newest ticks first, then the saved
// done items in their order.
// Both hold indices into `saved`.
void splitTasks(const TaskList& saved, const PendingTicks& pending, std::vector<size_t>& open,
                std::vector<size_t>& done);

enum class TickAction { Send, Drop };
// What sync does with a tick, given the task's done state on the server
// now (nullopt: deleted).
TickAction resolveTick(const Tick& tick, std::optional<bool> serverDone);

// Writes a sent tick into a saved copy, so it stays done (or not done)
// offline. False when the task is not in the list.
bool applySent(TaskList& saved, int64_t taskId, bool done);

}  // namespace vikunja
