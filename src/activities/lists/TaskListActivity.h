#pragma once

#include <VikunjaModel.h>

#include <cstdint>
#include <vector>

#include "activities/UiListActivity.h"

// One Vikunja project's items. The open view lists what is left, then a
// "Completed" row, and Back goes to the list of lists. The done view lists
// ticked items, newest first, with "Load more" when another page may
// follow. Ticking is saved to the SD card at once and sent on the next sync.
class TaskListActivity final : public UiListActivity {
 public:
  TaskListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int64_t projectId, bool showDone);

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  enum class RowKind : uint8_t { Task, Completed, LoadMore, Empty };
  struct Row {
    RowKind kind;
    size_t task;  // index into tasks, for RowKind::Task
  };

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void drawChrome() override;
  void onBackButton() override;

  // tasks, rows and status are read by the render task: replace them only
  // under the render lock (network work runs on copies).
  // With `replacement`, it becomes `tasks` in the same locked step as the
  // rows that index into it.
  void rebuildRows(vikunja::TaskList* replacement = nullptr);
  void setBusy();
  void runSync();
  void loadMore();
  void tick(size_t taskIndex);
  void leaveTo(std::unique_ptr<Activity> next);
  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);

  const int64_t projectId;
  const bool showDone;
  vikunja::TaskList tasks;
  std::vector<Row> rows;
  bool syncPending = false;
  bool syncing = false;
  bool moreDone = false;
  int nextDonePage = 2;
  bool stayingInLists = false;
  char title[96] = {};
  char status[96] = {};
  char rowBuf[256] = {};
};
