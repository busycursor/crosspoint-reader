#include "ControlActivity.h"

#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/icons/customListIcons.h"
#include "components/icons/listIcons.h"
#include "homeassistant/HaConfigStore.h"
#include "homeassistant/HaLink.h"
#include "homeassistant/HaSession.h"
#include "homeassistant/HaStateCache.h"

namespace fui = freeink::ui;

namespace {
constexpr const char* kLayoutCachePath = "/.crosspoint/homeassistant-layout.json";
constexpr size_t kMaxLayoutBytes = 64 * 1024;
constexpr unsigned long kRepaintMinMs = 5000;  // live updates batch at most this often
constexpr unsigned long kRecentTapMs = 10000;  // a tapped entity's updates skip the batching this long
constexpr int16_t kCardHeight = 56;
constexpr int16_t kSliderHeight = 56;
constexpr int HA_BRIGHTNESS_STEP = 10;

constexpr fui::ActionId ACTION_CARD = 1;    // value = item index
constexpr fui::ActionId ACTION_SLIDER = 2;  // the one slider being dragged is tracked by row
constexpr fui::ActionId ACTION_DOWN = 3;    // value = item index
constexpr fui::ActionId ACTION_UP = 4;      // value = item index

// Slider rows are distinguished by action id (drags carry no value).
constexpr fui::ActionId ACTION_SLIDER_BASE = 10;
constexpr int kMaxSlidersPerPage = 6;

uint8_t percentFromPermille(const int16_t permille) {
  int value = (static_cast<int>(permille) * 100 + 500) / 1000;
  return static_cast<uint8_t>(std::max(0, std::min(100, value)));
}
}  // namespace

ControlActivity::ControlActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Control", renderer, mappedInput), UiAppHost(renderer) {}

void ControlActivity::onEnter() {
  Activity::onEnter();
  resetUi();
  app.on(ACTION_CARD, &ControlActivity::onCardEvent, this);
  app.on(ACTION_DOWN, &ControlActivity::onStepEvent, this);
  app.on(ACTION_UP, &ControlActivity::onStepEvent, this);
  for (int i = 0; i < kMaxSlidersPerPage; ++i) {
    app.on(static_cast<fui::ActionId>(ACTION_SLIDER_BASE + i), &ControlActivity::onSliderEvent, this);
  }
  app.setScreen(&ControlActivity::screenBuilder, this);
  loadCachedLayout();
  // Opening the page asks for live data: the session joins the home network.
  session = HaSession::start();
  shownStatus = ha::LinkStatus::Working;
  requestUpdate();
}

void ControlActivity::onExit() {
  if (session) session->stop();
  session.reset();
  ha::saveStateCacheIfDirty();
  Activity::onExit();
}

void ControlActivity::loadCachedLayout() {
  std::string json;
  if (Storage.exists(kLayoutCachePath) && Storage.readFileToString("HA", kLayoutCachePath, kMaxLayoutBytes, json)) {
    setLayout(json, false);
  }
}

void ControlActivity::setLayout(const std::string& json, const bool save) {
  ha::Dashboard d;
  if (!ha::parseDashboard(json, 0, d)) {
    LOG_ERR("HA", "HA dashboard: unreadable layout");
    return;
  }
  dash = std::move(d);
  items.clear();
  for (int s = 0; s < static_cast<int>(dash.sections.size()); ++s) {
    if (!dash.sections[s].title.empty()) items.push_back(Item{s, -1, 0});
    for (int c = 0; c < static_cast<int>(dash.sections[s].cards.size()); ++c) items.push_back(Item{s, c, 0});
  }
  pageStarts.clear();  // recomputed with real metrics on the next build
  if (save) {
    Storage.ensureDirectoryExists("/.crosspoint");
    if (!Storage.writeFile(kLayoutCachePath, String(json.c_str()))) LOG_ERR("HA", "HA layout cache: write failed");
  }
}

const ha::Card* ControlActivity::cardAt(const int itemIndex) const {
  if (itemIndex < 0 || itemIndex >= static_cast<int>(items.size()) || items[itemIndex].card < 0) return nullptr;
  return &dash.sections[items[itemIndex].section].cards[items[itemIndex].card];
}

void ControlActivity::paginate(const int16_t available) {
  pageStarts.clear();
  int used = available;  // forces a page at the first item
  for (int i = 0; i < static_cast<int>(items.size()); ++i) {
    // A section title never ends a page on its own.
    int need = items[i].height;
    if (items[i].card < 0 && i + 1 < static_cast<int>(items.size())) need += items[i + 1].height;
    if (used + need > available) {
      pageStarts.push_back(i);
      used = 0;
    }
    used += items[i].height;
  }
  if (pageStarts.empty()) pageStarts.push_back(0);
  page = std::min(page, static_cast<int>(pageStarts.size()) - 1);
}

void ControlActivity::screenBuilder(UiScreen& screen, void* user) {
  static_cast<ControlActivity*>(user)->buildScreen(screen);
}

void ControlActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = screen.theme();
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), static_cast<int16_t>(metrics.contentSidePadding),
      static_cast<int16_t>(metrics.buttonHintsHeight), static_cast<int16_t>(metrics.contentSidePadding)});
  const int16_t line = screen.target().lineHeight(theme.smallText.font);

  // Why the cards may be stale, or nothing when they are live.
  const char* why = nullptr;
  if (shownStatus == ha::LinkStatus::NoNetwork) why = tr(STR_HA_NO_NETWORK);
  if (shownStatus == ha::LinkStatus::NoAnswer) why = tr(STR_HA_NO_ANSWER);
  if (shownStatus == ha::LinkStatus::TokenRejected) why = tr(STR_HA_TOKEN_REJECTED);
  const fui::Rect statusRow = screen.takeTop(line, theme.spaceSm);
  if (why) {
    screen.target().text(statusRow, why, theme.smallText);
  } else if (items.empty()) {
    screen.target().text(statusRow, tr(STR_LOADING), theme.smallText);
  }

  if (items.empty()) return;
  for (auto& item : items) {
    const ha::Card* card = item.card < 0 ? nullptr : &dash.sections[item.section].cards[item.card];
    if (!card) {
      item.height = static_cast<int16_t>(line + theme.spaceMd + theme.spaceSm);
    } else if (card->kind == ha::CardKind::Weather) {
      item.height = static_cast<int16_t>(2 * line + 3 * theme.spaceSm + theme.spaceSm);
    } else if (card->slider) {
      item.height = static_cast<int16_t>(line + theme.spaceMd + kSliderHeight + 2 * theme.spaceMd);
    } else {
      item.height = static_cast<int16_t>(kCardHeight + theme.spaceSm);
    }
  }
  const int16_t pageLine = static_cast<int16_t>(line + theme.spaceSm);
  if (pageStarts.empty()) paginate(static_cast<int16_t>(screen.contentRect().height - pageLine));

  const int first = pageStarts[page];
  const int last =
      page + 1 < static_cast<int>(pageStarts.size()) ? pageStarts[page + 1] : static_cast<int>(items.size());
  int sliders = 0;
  for (int i = first; i < last; ++i) {
    const Item& item = items[i];
    if (item.card < 0) {
      screen.spacer(theme.spaceMd);
      fui::TextStyle title = theme.smallText;
      title.bold = true;
      screen.target().text(screen.takeTop(line, theme.spaceSm), dash.sections[item.section].title.c_str(), title);
      continue;
    }
    const ha::Card& card = dash.sections[item.section].cards[item.card];
    if (card.slider && sliders >= kMaxSlidersPerPage) {
      // Out of slider ids on this page: draw it as a plain card.
      ha::Card plain = card;
      plain.slider = false;
      drawCard(screen, plain, i);
      continue;
    }
    if (card.slider) ++sliders;
    drawCard(screen, card, i);
  }

  if (pageStarts.size() > 1) {
    char pages[16];
    snprintf(pages, sizeof(pages), "%d / %d", page + 1, static_cast<int>(pageStarts.size()));
    fui::TextStyle footer = theme.smallText;
    footer.align = fui::TextAlign::Center;
    screen.target().text(screen.takeBottom(line), pages, footer);
  }
}

void ControlActivity::drawCard(UiScreen& screen, const ha::Card& card, const int itemIndex) {
  const auto& theme = screen.theme();
  const auto& cache = ha::stateCache();
  const auto it = cache.find(card.entity);
  const ha::EntityState none{};
  const ha::EntityState& s = it != cache.end() ? it->second : none;
  const std::string name = !card.name.empty() ? card.name : (!s.name.empty() ? s.name : card.entity);
  const bool pending = card.entity == pendingEntity;
  const std::string value = pending ? "..." : (it == cache.end() ? "" : ha::displayValue(card.entity, s));
  const int16_t line = screen.target().lineHeight(theme.smallText.font);
  const fui::Paint ink = fui::Paint::solid(fui::Color::Black);

  if (card.kind == ha::CardKind::Weather) {
    const fui::Rect box = screen.takeTop(static_cast<int16_t>(2 * line + 3 * theme.spaceSm), theme.spaceSm);
    screen.target().stroke(box, ink, 2, theme.controlRadius);
    fui::TextStyle body = theme.smallText;
    body.bold = true;
    screen.target().text(
        fui::Rect{static_cast<int16_t>(box.x + theme.spaceMd), static_cast<int16_t>(box.y + theme.spaceSm),
                  static_cast<int16_t>(box.width - 2 * theme.spaceMd), line},
        value.empty() ? name.c_str() : value.c_str(), body);
    screen.target().text(
        fui::Rect{static_cast<int16_t>(box.x + theme.spaceMd), static_cast<int16_t>(box.y + 2 * theme.spaceSm + line),
                  static_cast<int16_t>(box.width - 2 * theme.spaceMd), line},
        s.forecast.c_str(), theme.smallText);
    return;
  }

  if (card.slider) {
    const bool on = s.state == "on";
    const uint8_t level =
        dragCard == itemIndex ? dragLevel : static_cast<uint8_t>(on && s.brightnessPct > 0 ? s.brightnessPct : 0);
    char pct[16];
    if (pending) {
      snprintf(pct, sizeof(pct), "...");
    } else if (it == cache.end()) {
      pct[0] = '\0';
    } else if (on) {
      snprintf(pct, sizeof(pct), "%u%%", static_cast<unsigned>(level));
    } else {
      snprintf(pct, sizeof(pct), "%s", value.c_str());
    }
    // Which slider id this card got on this page (counted in build order).
    int sliderIndex = 0;
    for (int i = pageStarts[page]; i < itemIndex; ++i) {
      const ha::Card* c = cardAt(i);
      if (c && c->slider) ++sliderIndex;
    }
    rowProps.label = name.c_str();
    rowProps.value = pct;
    rowProps.sliderValue = level;
    rowProps.sliderAction = static_cast<fui::ActionId>(ACTION_SLIDER_BASE + sliderIndex);
    rowProps.decrement = ACTION_DOWN;
    rowProps.increment = ACTION_UP;
    rowProps.decrementValue = rowProps.incrementValue = static_cast<int16_t>(itemIndex);
    rowProps.toggleAction = ACTION_CARD;
    rowProps.toggleValue = static_cast<int16_t>(itemIndex);
    rowProps.toggleIcon = fui::bitmapFromIcon(on ? icon_sun_filled_32 : icon_sun_32);
    rowProps.labelText = theme.smallText;
    rowProps.labelText.bold = true;
    rowProps.valueText = theme.smallText;
    screen.sliderRow(rowProps, kSliderHeight);
    screen.spacer(theme.spaceMd);
    return;
  }

  ha::ServiceCall tap;
  const bool tappable = ha::tapAction(card.entity, tap);
  // Like the thing itself: on is white (lit), off is black.
  const bool off = ha::isToggleable(card.entity) && s.state == "off";
  const fui::Rect box = screen.takeTop(kCardHeight, theme.spaceSm);
  if (off) {
    screen.target().fill(box, ink, theme.controlRadius);
  } else if (tappable) {
    screen.target().stroke(box, ink, 2, theme.controlRadius);
  } else {
    screen.target().stroke(box, fui::Paint::solid(fui::Color::Black), 1, theme.controlRadius);
  }
  fui::TextStyle label = theme.smallText;
  label.color = off ? fui::Color::White : fui::Color::Black;
  fui::TextStyle val = label;
  val.align = fui::TextAlign::Right;
  label.bold = true;
  const fui::Rect text{static_cast<int16_t>(box.x + theme.spaceMd),
                       static_cast<int16_t>(box.y + (box.height - line) / 2),
                       static_cast<int16_t>(box.width - 2 * theme.spaceMd), line};
  screen.target().text(fui::Rect{text.x, text.y, static_cast<int16_t>(text.width * 2 / 3), text.height}, name.c_str(),
                       label);
  screen.target().text(text, value.c_str(), val);
  if (tappable) screen.frame().hit(box, ACTION_CARD, static_cast<int16_t>(itemIndex), fui::InputTouch);
}

void ControlActivity::onCardEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<ControlActivity*>(user);
  const ha::Card* card = self->cardAt(event.value);
  ha::ServiceCall call;
  if (card && ha::tapAction(card->entity, call)) self->send(call);
}

void ControlActivity::onStepEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<ControlActivity*>(user);
  const ha::Card* card = self->cardAt(event.value);
  if (!card) return;
  const auto& cache = ha::stateCache();
  const auto it = cache.find(card->entity);
  const int current = it != cache.end() && it->second.brightnessPct > 0 ? it->second.brightnessPct : 0;
  const int step = event.action == ACTION_UP ? HA_BRIGHTNESS_STEP : -HA_BRIGHTNESS_STEP;
  self->send(ha::brightnessCall(card->entity, std::max(0, std::min(100, current + step))));
}

void ControlActivity::onSliderEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<ControlActivity*>(user);
  if (event.dragPermille < 0 || self->pageStarts.empty()) return;
  const int wanted = event.action - ACTION_SLIDER_BASE;
  int sliderIndex = 0;
  const int last = self->page + 1 < static_cast<int>(self->pageStarts.size()) ? self->pageStarts[self->page + 1]
                                                                              : static_cast<int>(self->items.size());
  for (int i = self->pageStarts[self->page]; i < last; ++i) {
    const ha::Card* c = self->cardAt(i);
    if (!c || !c->slider) continue;
    if (sliderIndex++ == wanted) {
      self->dragCard = i;
      self->dragLevel = percentFromPermille(event.dragPermille);
      return;
    }
  }
}

void ControlActivity::send(const ha::ServiceCall& call) {
  recentEntity = call.entityId;
  recentUntilMs = millis() + kRecentTapMs;
  if (haLink.submit(call)) {
    pendingEntity = call.entityId;
    hasQueued = false;
  } else {
    queued = call;
    hasQueued = true;
  }
  requestUpdate();
}

void ControlActivity::pollLink() {
  if (!pendingEntity.empty() && !haLink.busy()) {
    std::string entity;
    ha::EntityState fresh;
    if (haLink.takeResult(entity, fresh)) ha::updateCachedState(entity, fresh);
    pendingEntity.clear();
    requestUpdate();  // a tap's own result shows at once
    lastPaintMs = millis();
  }
  if (hasQueued && !haLink.busy()) send(queued);
}

void ControlActivity::pollSession() {
  if (!session) return;
  HaSession::Update update;
  if (session->poll(update)) {
    if (!update.layoutJson.empty()) {
      setLayout(update.layoutJson, true);
      repaintDue = true;
    }
    bool tapped = false;
    for (const auto& [id, state] : update.changes) {
      if (!ha::updateCachedState(id, state)) continue;
      repaintDue = true;
      // The control just used: show its update now, not at the next batch.
      if (id == recentEntity && static_cast<long>(recentUntilMs - millis()) > 0) tapped = true;
    }
    if (tapped && pendingEntity.empty()) {
      repaintDue = false;
      lastPaintMs = millis();
      requestUpdate();
    }
  }
  const ha::LinkStatus status = session->status();
  if (status != shownStatus && status != ha::LinkStatus::Working) {
    shownStatus = status;
    repaintDue = true;
  }
  if (repaintDue && (items.empty() || millis() - lastPaintMs >= kRepaintMinMs || dash.sections.empty())) {
    repaintDue = false;
    lastPaintMs = millis();
    requestUpdate();
  }
}

void ControlActivity::loop() {
  pollLink();
  pollSession();

  const auto touch = routeTouch(mappedInput, false, /*routeHeld=*/true);
  if (touch.routed) {
    if (app.invalidated()) requestUpdate();
    if (touch) {
      if (touch.event.dragPermille >= 0) draggingSlider = true;
      return;
    }
    const auto swipe = mappedInput.wasSwipe();
    if (!draggingSlider && swipe == MappedInputManager::SwipeDir::Up &&
        page + 1 < static_cast<int>(pageStarts.size())) {
      ++page;
      requestUpdate();
      return;
    }
    if (!draggingSlider && swipe == MappedInputManager::SwipeDir::Down && page > 0) {
      --page;
      requestUpdate();
      return;
    }
  }
  if (draggingSlider) {
    if (!touch.snap.touchHeld) {
      draggingSlider = false;
      if (const ha::Card* card = cardAt(dragCard)) send(ha::brightnessCall(card->entity, dragLevel));
      dragCard = -1;
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goHome();
    return;
  }
  // Side buttons turn pages as they do in a book (Settings > Controls); with
  // side page buttons disabled they still page, as Down/Up.
  const bool sideOff = SETTINGS.sideButtonLayout == CrossPointSettings::SIDE_BUTTONS_DISABLED;
  const bool next =
      mappedInput.wasReleased(sideOff ? MappedInputManager::Button::Down : MappedInputManager::Button::PageForward) ||
      mappedInput.wasReleased(MappedInputManager::Button::Right);
  const bool prev =
      mappedInput.wasReleased(sideOff ? MappedInputManager::Button::Up : MappedInputManager::Button::PageBack) ||
      mappedInput.wasReleased(MappedInputManager::Button::Left);
  if (next || prev)
    LOG_INF("HA", "Control: %s pressed on page %d/%d", next ? "next" : "previous", page + 1,
            static_cast<int>(pageStarts.size()));
  if (next && page + 1 < static_cast<int>(pageStarts.size())) {
    ++page;
    requestUpdate();
  } else if (prev && page > 0) {
    --page;
    requestUpdate();
  }
}

void ControlActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const char* title = !dash.title.empty() ? dash.title.c_str() : tr(STR_CONTROL);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, title);
  renderUi();
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
