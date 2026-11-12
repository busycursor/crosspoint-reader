#include "TaskListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <memory>

#include "ListsActivity.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "vikunja/VikunjaStore.h"

namespace fui = freeink::ui;

TaskListActivity::TaskListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const int64_t projectId,
                                   const bool showDone)
    : UiListActivity("ListItems", renderer, mappedInput), projectId(projectId), showDone(showDone) {}

void TaskListActivity::onEnter() {
  UiListActivity::onEnter();
  stayingInLists = false;
  vikunja::TaskList saved;
  if (!vikunja::loadTasks(projectId, saved)) saved.clear();
  size_t doneRows = 0;
  for (size_t i = 0; i < saved.size(); ++i) doneRows += saved.done(i) ? 1 : 0;
  {
    // The base onEnter already asked for a frame.
    RenderLock lock(*this);
    const vikunja::Project* p = vikunja::findProject(vikunja::projects(), projectId);
    copyTitle(title, sizeof(title), "", p ? std::string_view(p->title) : std::string_view{});
    tasks = std::move(saved);
    moreDone = doneRows >= vikunja::pageSize();
  }
  rebuildRows();
  // Once per visit: the full sync if this is the first screen of it (the
  // favourite), else this list's items the first time it is opened.
  syncPending = !vikunja::syncedThisVisit() || !vikunja::fetchedThisVisit(projectId);
  requestUpdate();
}

void TaskListActivity::onExit() {
  if (!stayingInLists) vikunja::endVisit();
  UiListActivity::onExit();
}

void TaskListActivity::loop() {
  if (syncPending) {
    syncPending = false;
    runSync();
    return;
  }
  UiListActivity::loop();
}

void TaskListActivity::setBusy() {
  {
    RenderLock lock(*this);
    syncing = true;
    formatListsStatus(status, sizeof(status), true);
  }
  requestUpdateAndWait();
}

void TaskListActivity::runSync() {
  setBusy();
  vikunja::TaskList fresh;
  const vikunja::Status s =
      vikunja::syncedThisVisit() ? vikunja::refreshProject(projectId, &fresh) : vikunja::sync(projectId, &fresh);
  syncing = false;
  if (!vikunja::findProject(vikunja::projects(), projectId)) {
    // Deleted on the server: back to the project list.
    leaveTo(std::make_unique<ListsActivity>(renderer, mappedInput, false));
    return;
  }
  if (s == vikunja::Status::Ok || s == vikunja::Status::Incomplete) {
    size_t doneRows = 0;
    for (size_t i = 0; i < fresh.size(); ++i) doneRows += fresh.done(i) ? 1 : 0;
    nextDonePage = 2;
    moreDone = doneRows >= vikunja::pageSize();
    rebuildRows(&fresh);
  } else {
    // A sync that failed partway may already have sent ticks and written them
    // into the saved copy; show that rather than the copy from before.
    vikunja::TaskList saved;
    rebuildRows(vikunja::loadTasks(projectId, saved) ? &saved : nullptr);
  }
  requestUpdate();
}

void TaskListActivity::loadMore() {
  setBusy();
  vikunja::TaskList grown = tasks;
  bool more = false;
  const bool ok = vikunja::loadMoreDone(projectId, &nextDonePage, grown, &more) == vikunja::Status::Ok;
  syncing = false;
  if (ok) moreDone = more;
  rebuildRows(ok ? &grown : nullptr);
  requestUpdate();
}

void TaskListActivity::rebuildRows(vikunja::TaskList* replacement) {
  std::vector<size_t> open, done;
  vikunja::splitTasks(replacement ? *replacement : tasks, vikunja::pending(), open, done);
  const std::vector<size_t>& shown = showDone ? done : open;
  std::vector<Row> fresh;
  fresh.reserve(shown.size() + 2);
  for (const size_t i : shown) fresh.push_back({RowKind::Task, i});
  if (showDone) {
    if (moreDone) fresh.push_back({RowKind::LoadMore, 0});
  } else {
    if (shown.empty()) fresh.push_back({RowKind::Empty, 0});
    fresh.push_back({RowKind::Completed, 0});
  }
  RenderLock lock(*this);
  if (replacement) tasks = std::move(*replacement);
  rows = std::move(fresh);
  formatListsStatus(status, sizeof(status), syncing);
  if (nav.selected >= static_cast<int>(rows.size())) moveSelectionTo(static_cast<int>(rows.size()) - 1);
}

int TaskListActivity::listCount() const { return static_cast<int>(rows.size()); }

void TaskListActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  char heading[128];
  snprintf(heading, sizeof(heading), showDone ? "%s: %s" : "%s", title, tr(STR_COMPLETED));
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, heading,
                 status[0] ? status : nullptr);
}

void TaskListActivity::provideRow(void* ctx, const uint16_t index, fui::ListItem& item) {
  auto* self = static_cast<TaskListActivity*>(ctx);
  if (index >= self->rows.size()) return;
  const Row& row = self->rows[index];
  item.actionValue = static_cast<int16_t>(index);
  switch (row.kind) {
    case RowKind::Task:
      copyTitle(self->rowBuf, sizeof(self->rowBuf), "", self->tasks.title(row.task));
      item.label = self->rowBuf;
      break;
    case RowKind::Completed:
      item.label = tr(STR_COMPLETED);
      item.bold = true;
      break;
    case RowKind::LoadMore:
      item.label = tr(STR_LISTS_LOAD_MORE);
      break;
    case RowKind::Empty:
      item.label = tr(STR_LISTS_EMPTY);
      item.enabled = false;
      break;
  }
}

void TaskListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  fui::ListProps props;
  props.rowProvider = &TaskListActivity::provideRow;
  props.rowProviderCtx = this;
  props.count = static_cast<uint16_t>(rows.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  fui::TextStyle label = screen.theme().bodyText;
  label.maxLines = 2;
  props.labelText = label;
  syncListViewport(screen, props);
  screen.list(props);
}

void TaskListActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount() || syncing) return;
  const Row& row = rows[index];
  switch (row.kind) {
    case RowKind::Task:
      tick(row.task);
      break;
    case RowKind::Completed:
      app.clearTapFlash();
      leaveTo(std::make_unique<TaskListActivity>(renderer, mappedInput, projectId, true));
      break;
    case RowKind::LoadMore:
      loadMore();
      break;
    case RowKind::Empty:
      break;
  }
}

// Saved before the screen changes, so a tick survives a power cut.
void TaskListActivity::tick(const size_t taskIndex) {
  auto& waiting = vikunja::pending();
  const int64_t id = tasks.id(taskIndex);
  const bool serverDone = tasks.done(taskIndex);
  bool shownDone = serverDone;
  for (const auto& t : waiting.all()) {
    if (t.taskId == id) shownDone = t.done;
  }
  waiting.toggle(id, !shownDone, serverDone, projectId);
  if (!vikunja::savePending()) {
    waiting.toggle(id, shownDone, serverDone, projectId);
    LOG_ERR("VIK", "tick not saved; undone");
    return;
  }
  rebuildRows();
  requestUpdate();
}

void TaskListActivity::leaveTo(std::unique_ptr<Activity> next) {
  stayingInLists = true;
  activityManager.replaceActivity(std::move(next));
}

void TaskListActivity::onBackButton() {
  if (syncing) return;
  if (showDone) {
    leaveTo(std::make_unique<TaskListActivity>(renderer, mappedInput, projectId, false));
  } else {
    leaveTo(std::make_unique<ListsActivity>(renderer, mappedInput, false, projectId));
  }
}
