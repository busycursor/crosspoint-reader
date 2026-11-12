#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "activities/UiListActivity.h"

// Home -> Lists: the Vikunja projects (sub-projects indented under their
// parent), then a Sync row. Opens straight into the favourite: the list
// picked in Settings, else the first one starred in Vikunja.
// See docs/superpowers/specs/2026-11-11-vikunja-lists-design.md.
class ListsActivity final : public UiListActivity {
 public:
  // openFavourite: false when coming back from a list, so Back from a list
  // does not bounce straight into the favourite again. selectProject: the
  // row to start on (the list just left).
  ListsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool openFavourite = true,
                int64_t selectProject = 0);

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void drawChrome() override;
  void onBackButton() override;

  // Rows and status are read by the render task: change them only here,
  // under the render lock.
  void rebuildRows();
  void runSync();
  void openProject(int64_t projectId);
  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);

  const bool openFavourite;
  const int64_t selectProject;
  bool syncPending = false;
  bool entrySync = false;  // the sync on entering, which may open the favourite
  bool syncing = false;
  // Leaving to another Lists screen keeps Wi-Fi up; anything else releases it.
  bool stayingInLists = false;
  struct Row {
    int64_t projectId;  // 0: the Sync row
    uint8_t depth;
    std::string title;  // a copy: sync replaces the project list meanwhile
  };
  std::vector<Row> rows;
  char status[96] = {};
  char rowBuf[160] = {};
};

// The status line shared by the Lists screens: the last sync's problem, then
// the number of ticks not yet sent. Empty when there is nothing to say.
void formatListsStatus(char* out, size_t size, bool syncing);
// Copies a UTF-8 title into out, cut on a character boundary, NUL-terminated.
void copyTitle(char* out, size_t size, const char* prefix, std::string_view title);
