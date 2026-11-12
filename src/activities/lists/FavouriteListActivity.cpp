#include "FavouriteListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "ListsActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "vikunja/VikunjaStore.h"

namespace fui = freeink::ui;

FavouriteListActivity::FavouriteListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("FavouriteList", renderer, mappedInput) {}

void FavouriteListActivity::onEnter() {
  UiListActivity::onEnter();
  RenderLock lock(*this);  // the base onEnter already asked for a frame
  const auto& projects = vikunja::projects();
  choices.clear();
  choices.reserve(projects.size() + 1);
  choices.push_back(0);
  int selected = 0;
  for (const auto& p : projects) {
    if (p.archived) continue;
    if (p.id == SETTINGS.vikunjaFavourite) selected = static_cast<int>(choices.size());
    choices.push_back(p.id);
  }
  // A choice that no longer exists shows "Starred in Vikunja" selected.
  nav.reset(selected);
}

int FavouriteListActivity::listCount() const { return static_cast<int>(choices.size()); }

const char* FavouriteListActivity::headerTitle() const { return tr(STR_FAVOURITE_LIST); }

void FavouriteListActivity::provideRow(void* ctx, const uint16_t index, fui::ListItem& item) {
  auto* self = static_cast<FavouriteListActivity*>(ctx);
  if (index >= self->choices.size()) return;
  const int64_t id = self->choices[index];
  item.actionValue = static_cast<int16_t>(index);
  const vikunja::Project* p = id ? vikunja::findProject(vikunja::projects(), id) : nullptr;
  if (!p) {
    item.label = tr(STR_LISTS_STARRED);
  } else {
    copyTitle(self->rowBuf, sizeof(self->rowBuf), "", p->title);
    item.label = self->rowBuf;
  }
  const int64_t current =
      vikunja::findProject(vikunja::projects(), SETTINGS.vikunjaFavourite) ? SETTINGS.vikunjaFavourite : 0;
  if (id == current) item.value = tr(STR_SELECTED);
}

void FavouriteListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  fui::ListProps props;
  props.rowProvider = &FavouriteListActivity::provideRow;
  props.rowProviderCtx = this;
  props.count = static_cast<uint16_t>(choices.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  fui::TextStyle label = screen.theme().smallText;
  label.maxLines = 1;
  props.labelText = label;
  syncListViewport(screen, props);
  screen.list(props);
}

void FavouriteListActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  SETTINGS.vikunjaFavourite = choices[index];
  SETTINGS.saveToFile();
  finish();
}
