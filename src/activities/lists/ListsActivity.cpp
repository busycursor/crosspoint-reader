#include "ListsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "TaskListActivity.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "vikunja/VikunjaStore.h"

namespace fui = freeink::ui;

void formatListsStatus(char* out, const size_t size, const bool syncing) {
  out[0] = '\0';
  const char* problem = nullptr;
  if (syncing) {
    problem = tr(STR_LISTS_SYNCING);
  } else {
    switch (vikunja::lastStatus()) {
      case vikunja::Status::Ok:
        break;
      case vikunja::Status::Offline:
        problem = tr(STR_LISTS_OFFLINE);
        break;
      case vikunja::Status::TokenRejected:
        problem = tr(STR_LISTS_TOKEN_REJECTED);
        break;
      case vikunja::Status::AccessRefused:
        problem = tr(STR_LISTS_ACCESS_REFUSED);
        break;
      case vikunja::Status::Incomplete:
        problem = tr(STR_LISTS_INCOMPLETE);
        break;
      case vikunja::Status::ServerDown:
        problem = tr(STR_LISTS_SYNC_FAILED);
        break;
    }
  }
  const int waiting = static_cast<int>(vikunja::pending().all().size());
  char count[48] = {};
  if (waiting > 0) snprintf(count, sizeof(count), tr(STR_LISTS_PENDING_N), waiting);
  if (problem && count[0]) {
    snprintf(out, size, "%s, %s", problem, count);
  } else {
    snprintf(out, size, "%s", problem ? problem : count);
  }
}

void copyTitle(char* out, const size_t size, const char* prefix, std::string_view title) {
  const size_t prefixLen = std::min(strlen(prefix), size - 1);
  memcpy(out, prefix, prefixLen);
  size_t n = std::min(title.size(), size - 1 - prefixLen);
  // Do not cut a multi-byte character in half.
  while (n > 0 && n < title.size() && (static_cast<unsigned char>(title[n]) & 0xC0) == 0x80) --n;
  memcpy(out + prefixLen, title.data(), n);
  out[prefixLen + n] = '\0';
}

ListsActivity::ListsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const bool openFavourite,
                             const int64_t selectProject)
    : UiListActivity("Lists", renderer, mappedInput), openFavourite(openFavourite), selectProject(selectProject) {}

void ListsActivity::onEnter() {
  UiListActivity::onEnter();
  stayingInLists = false;
  const int64_t favourite = vikunja::favouriteProject(vikunja::projects(), SETTINGS.vikunjaFavourite);
  if (openFavourite && favourite != 0) {
    openProject(favourite);
    return;
  }
  rebuildRows();
  for (size_t i = 0; i < rows.size(); ++i) {
    if (selectProject != 0 && rows[i].projectId == selectProject) {
      RenderLock lock(*this);
      moveSelectionTo(static_cast<int>(i));
    }
  }
  syncPending = !vikunja::syncedThisVisit();
  // A list starred since the last sync is opened once this sync finds it.
  entrySync = syncPending && openFavourite;
  requestUpdate();
}

void ListsActivity::onExit() {
  if (!stayingInLists) vikunja::endVisit();
  UiListActivity::onExit();
}

void ListsActivity::loop() {
  if (syncPending) {
    syncPending = false;
    runSync();
    return;
  }
  UiListActivity::loop();
}

void ListsActivity::runSync() {
  {
    RenderLock lock(*this);
    syncing = true;
    formatListsStatus(status, sizeof(status), true);
  }
  requestUpdateAndWait();
  vikunja::sync(0, nullptr);
  syncing = false;
  if (std::exchange(entrySync, false)) {
    const int64_t favourite = vikunja::favouriteProject(vikunja::projects(), SETTINGS.vikunjaFavourite);
    if (favourite != 0) {
      // Fetch its items under the same "Syncing...", so the list opens with
      // nothing left to load (it is marked fetched for this visit).
      vikunja::TaskList items;
      vikunja::refreshProject(favourite, &items);
      openProject(favourite);
      return;
    }
  }
  rebuildRows();
  requestUpdate();
}

void ListsActivity::rebuildRows() {
  const auto& projects = vikunja::projects();
  std::vector<Row> fresh;
  fresh.reserve(projects.size() + 1);
  // Depth-first: each project followed by its sub-projects.
  auto addChildren = [&](auto& self, const int64_t parent, const uint8_t depth) -> void {
    for (const auto& p : projects) {
      if (p.archived || p.parentId != parent) continue;
      fresh.push_back({p.id, depth, p.title});
      if (depth < 4) self(self, p.id, static_cast<uint8_t>(depth + 1));
    }
  };
  addChildren(addChildren, 0, 0);
  // Sub-projects whose parent is archived or missing still get a row.
  for (const auto& p : projects) {
    if (p.archived || p.parentId == 0) continue;
    bool shown = false;
    for (const auto& r : fresh) shown = shown || r.projectId == p.id;
    if (!shown) fresh.push_back({p.id, 0, p.title});
  }
  fresh.push_back({0, 0, {}});
  RenderLock lock(*this);
  rows = std::move(fresh);
  formatListsStatus(status, sizeof(status), false);
  if (nav.selected >= static_cast<int>(rows.size())) moveSelectionTo(static_cast<int>(rows.size()) - 1);
}

int ListsActivity::listCount() const { return static_cast<int>(rows.size()); }

const char* ListsActivity::headerTitle() const { return tr(STR_LISTS); }

void ListsActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, tr(STR_LISTS),
                 status[0] ? status : nullptr);
}

void ListsActivity::provideRow(void* ctx, const uint16_t index, fui::ListItem& item) {
  auto* self = static_cast<ListsActivity*>(ctx);
  if (index >= self->rows.size()) return;
  const Row& row = self->rows[index];
  item.actionValue = static_cast<int16_t>(index);
  if (row.projectId == 0) {
    item.label = tr(STR_LISTS_SYNC);
    return;
  }
  static constexpr const char* kIndent[] = {"", "    ", "        ", "            ", "                "};
  copyTitle(self->rowBuf, sizeof(self->rowBuf), kIndent[std::min<uint8_t>(row.depth, 4)], row.title);
  item.label = self->rowBuf;
}

void ListsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  fui::ListProps props;
  props.rowProvider = &ListsActivity::provideRow;
  props.rowProviderCtx = this;
  props.count = static_cast<uint16_t>(rows.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  fui::TextStyle label = screen.theme().smallText;
  label.maxLines = 1;
  props.labelText = label;
  syncListViewport(screen, props);
  screen.list(props);
}

void ListsActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount() || syncing) return;
  app.clearTapFlash();
  const Row& row = rows[index];
  if (row.projectId == 0) {
    vikunja::retryWifi();
    runSync();
    return;
  }
  openProject(row.projectId);
}

void ListsActivity::openProject(const int64_t projectId) {
  stayingInLists = true;
  activityManager.replaceActivity(std::make_unique<TaskListActivity>(renderer, mappedInput, projectId, false));
}

void ListsActivity::onBackButton() {
  if (syncing) return;
  activityManager.goHome(HomeMenuItem::LISTS);
}
