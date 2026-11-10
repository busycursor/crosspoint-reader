#include "FrontlightPanelActivity.h"

#include <BlePageTurner.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <iterator>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/icons/customListIcons.h"
#include "components/icons/listIcons.h"
#include "components/icons/readerToolbarIcons.h"
#include "homeassistant/HaConfigStore.h"
#include "homeassistant/HaLink.h"
#include "homeassistant/HaStateCache.h"
#include "util/SlideSwitch.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_BRIGHTNESS = 1;
constexpr fui::ActionId ACTION_WARMTH = 2;
constexpr fui::ActionId ACTION_TOGGLE = 3;
constexpr fui::ActionId ACTION_BRIGHTNESS_STEP = 4;
constexpr fui::ActionId ACTION_WARMTH_STEP = 5;
constexpr fui::ActionId ACTION_TILE = 6;  // value = tile index
// Home Assistant: one slider/step id per light row (sliders carry no value).
constexpr fui::ActionId ACTION_HA_SLIDER = 10;  // +row
constexpr fui::ActionId ACTION_HA_DOWN = 20;    // value = row
constexpr fui::ActionId ACTION_HA_UP = 21;      // value = row
constexpr fui::ActionId ACTION_HA_TOGGLE = 22;  // value = row
constexpr fui::ActionId ACTION_HA_BUTTON = 23;  // value = button
// A step sends a call, so step in tens, not the frontlight's single percent.
constexpr int HA_BRIGHTNESS_STEP = 10;

// iOS-style geometry. The panel is a card hanging from the top of the screen:
// a grabber, full-width slider pills, then a row of quick-setting icons. The chrome
// itself is fui::sheet / fui::sliderRow; these constants only
// size the bands, and computePanelBottom() mirrors them.
constexpr int16_t kPanelSideMargin = 16;
constexpr int16_t kGrabberHeight = 5;     // fui::SheetProps default, mirrored here
constexpr int16_t kSliderRowHeight = 56;  // the pill itself (finger-sized)
constexpr int16_t kQuickControlSize = kSliderRowHeight;
constexpr int16_t kQuickIconSize = 32;
// One percent per press, on the -/+ buttons and on the physical Left/Right keys
// alike (both repeat while held), so a level can be set exactly.
constexpr int BRIGHTNESS_STEP = 1;
// The dimmest setting is 1%, not 0: turning the light off is what the lamp
// button next to the slider is for, so a 0% "on" level would only be a second,
// worse way to reach the same place.
constexpr uint8_t MIN_BRIGHTNESS = 1;

uint8_t percentFromPermille(const int16_t permille) {
  int value = (static_cast<int>(permille) * 100 + 500) / 1000;
  if (value < 0) value = 0;
  if (value > 100) value = 100;
  return static_cast<uint8_t>(value);
}
}  // namespace

FrontlightPanelActivity::FrontlightPanelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FrontlightPanel", renderer, mappedInput), UiAppHost(renderer) {}

void FrontlightPanelActivity::onEnter() {
  Activity::onEnter();

  // A stored 0% predates the 1% floor (or came from the web settings): show it
  // as the floor rather than a level the slider can no longer produce. onExit
  // persists that, which is the intent — 0 is not a brightness any more.
  brightness = std::max(MIN_BRIGHTNESS, Frontlight.brightness());
  warmth = Frontlight.warmth();
  lightOn = Frontlight.isOn();
  lightOnChanged = false;

  // Seed the touch tile's restore mode from the live setting, so toggling off
  // and back on within this session returns to the mode the user had.
  if (SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_OFF) {
    touchModeRestore = SETTINGS.touchReaderControls;
  }

  resetUi();
  app.on(ACTION_BRIGHTNESS, &FrontlightPanelActivity::onBrightnessEvent, this);
  app.on(ACTION_WARMTH, &FrontlightPanelActivity::onWarmthEvent, this);
  app.on(ACTION_TOGGLE, &FrontlightPanelActivity::onToggleEvent, this);
  app.on(ACTION_BRIGHTNESS_STEP, &FrontlightPanelActivity::onBrightnessStepEvent, this);
  app.on(ACTION_WARMTH_STEP, &FrontlightPanelActivity::onWarmthStepEvent, this);
  app.on(ACTION_TILE, &FrontlightPanelActivity::onTileEvent, this);
  loadHaRows();
  for (int i = 0; i < haLightCount; ++i) {
    app.on(static_cast<fui::ActionId>(ACTION_HA_SLIDER + i), &FrontlightPanelActivity::onHaSliderEvent, this);
  }
  app.on(ACTION_HA_DOWN, &FrontlightPanelActivity::onHaStepEvent, this);
  app.on(ACTION_HA_UP, &FrontlightPanelActivity::onHaStepEvent, this);
  app.on(ACTION_HA_TOGGLE, &FrontlightPanelActivity::onHaToggleEvent, this);
  app.on(ACTION_HA_BUTTON, &FrontlightPanelActivity::onHaButtonEvent, this);
  app.setScreen(&FrontlightPanelActivity::panelScreen, this);
  requestUpdate();
}

void FrontlightPanelActivity::persistLightSettings() {
  if (!Frontlight.present()) return;
  // brightness/warmth are always restored unconditionally on boot (see
  // main.cpp), so they never diverge from SETTINGS at onEnter() — comparing
  // against SETTINGS here only fires on a genuine user change. lightOn has
  // no such guarantee (see lightOnChanged's declaration), so it's gated on
  // the user actually having touched it this session instead.
  const bool changed = SETTINGS.frontlightBrightness != brightness || SETTINGS.frontlightWarmth != warmth ||
                       (lightOnChanged && SETTINGS.frontlightOn != (lightOn ? 1 : 0));
  if (changed) {
    SETTINGS.frontlightBrightness = brightness;
    SETTINGS.frontlightWarmth = warmth;
    if (lightOnChanged) SETTINGS.frontlightOn = lightOn ? 1 : 0;
    SETTINGS.saveToFile();
  }
}

void FrontlightPanelActivity::onExit() {
  persistLightSettings();
  Activity::onExit();
}

void FrontlightPanelActivity::onBrightnessEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  self->brightness = std::max(MIN_BRIGHTNESS, percentFromPermille(event.dragPermille));
  Frontlight.setBrightness(self->brightness);
  if (!self->lightOn && !slideSwitchOwnsLight()) {
    self->lightOn = true;
    self->lightOnChanged = true;
    Frontlight.setOn(true);
  }
}

void FrontlightPanelActivity::onWarmthEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  self->warmth = percentFromPermille(event.dragPermille);
  Frontlight.setWarmth(self->warmth);
}

void FrontlightPanelActivity::onToggleEvent(const fui::ActionEvent&, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->toggleLight();
}

void FrontlightPanelActivity::onBrightnessStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustBrightness(event.value);
}

void FrontlightPanelActivity::onWarmthStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustWarmth(event.value);
}

void FrontlightPanelActivity::onTileEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->runTile(event.value);
}

void FrontlightPanelActivity::runTile(const int idx) {
  switch (idx) {
    case 0:  // Night mode (inverted output polarity, applied to the whole UI)
      SETTINGS.screenInverted = SETTINGS.screenInverted ? 0 : 1;
      SETTINGS.saveToFile();
      // Inversion rewrites every pixel; take the clean waveform so the panel
      // does not keep a ghost of the old polarity.
      cleanRefreshPending = true;
      requestUpdate();
      break;
    case 1:  // Ghost-cleanup refresh of the whole frame
      // Refreshing with the panel still up would clean a frame the user is
      // about to dismiss anyway: drop the panel first and let the repaint of
      // the screen underneath carry the clean waveform instead.
      renderer.promoteNextRefresh(HalDisplay::FULL_REFRESH);
      close();
      break;
    case 2:  // Cycle the reading orientation
      SETTINGS.orientation = static_cast<uint8_t>((SETTINGS.orientation + 1) % CrossPointSettings::ORIENTATION_COUNT);
      SETTINGS.saveToFile();
      // Only the setting changes: turning the renderer cropped the portrait-only
      // screens the panel opens over. The reader reflows on its next loop().
      requestUpdate();
      break;
    case 3:  // Touch reader controls (for reading with the palm on the glass)
      // Toggles the existing Settings -> Controls option, nothing lower-level:
      // that setting only governs the reader's tap/swipe handling, so the
      // panel's own gestures (including the swipe that reopens it) keep
      // working while it is off. Off remembers the mode (Tap/Swipe/Inverted
      // Tap) so toggling back does not stomp the user's choice.
      if (SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_OFF) {
        touchModeRestore = SETTINGS.touchReaderControls;
        SETTINGS.touchReaderControls = CrossPointSettings::TOUCH_READER_OFF;
      } else {
        SETTINGS.touchReaderControls = touchModeRestore;
      }
      SETTINGS.saveToFile();
      requestUpdate();
      break;
    case 4:  // Bluetooth page turner
      if (!bleturner::status().compiledIn) break;
      SETTINGS.ble.enabled = SETTINGS.ble.enabled ? 0 : 1;
      // The panel suspends the radio; tick() reconnects in an eligible screen.
      if (!SETTINGS.ble.enabled) bleturner::switchOff();
      SETTINGS.saveToFile();
      requestUpdate();
      break;
    default:
      break;
  }
}

void FrontlightPanelActivity::adjustBrightness(const int delta) {
  int next = static_cast<int>(brightness) + delta;
  if (next < MIN_BRIGHTNESS) next = MIN_BRIGHTNESS;
  if (next > 100) next = 100;
  if (next == brightness) return;
  brightness = static_cast<uint8_t>(next);
  Frontlight.setBrightness(brightness);
  if (!lightOn && !slideSwitchOwnsLight()) {
    lightOn = true;
    lightOnChanged = true;
    Frontlight.setOn(true);
  }
  requestUpdate();
}

void FrontlightPanelActivity::adjustWarmth(const int delta) {
  int next = static_cast<int>(warmth) + delta;
  if (next < 0) next = 0;
  if (next > 100) next = 100;
  if (next == warmth) return;
  warmth = static_cast<uint8_t>(next);
  Frontlight.setWarmth(warmth);
  requestUpdate();
}

void FrontlightPanelActivity::toggleLight() {
  lightOn = !lightOn;
  lightOnChanged = true;
  Frontlight.setOn(lightOn);
  requestUpdate();
}

void FrontlightPanelActivity::close() { finish(); }

bool FrontlightPanelActivity::handleHomeGesture() {
  close();
  return true;
}

void FrontlightPanelActivity::loop() {
  pollHa();
  const auto touch = routeTouch(mappedInput, false, /*routeHeld=*/true);
  if (touch.routed) {
    if (app.invalidated()) requestUpdate();
    if (touch) {
      if (touch.event.dragPermille >= 0) draggingSlider = true;
      return;
    }
    // Swipe up dismisses the sheet, the way it was pulled down from the top
    // edge. draggingSlider keeps a fast slider flick from closing it.
    if (!draggingSlider && mappedInput.wasSwipe() == MappedInputManager::SwipeDir::Up) {
      close();
      return;
    }
    // panelBottom > 0 guards the frame the sheet opens in: the release that
    // opened it (a status-bar tap) is still in the input snapshot when the panel
    // runs its first loop(), and panelBottom is only known once render() has
    // measured the layout — so at 0 that release read as "tapped below the
    // sheet" and closed it again before it was ever drawn.
    if (touch.snap.touchReleased && !draggingSlider && panelBottom > 0 && touch.snap.touchY >= panelBottom) {
      close();
      return;
    }
  }
  if (draggingSlider) {
    if (!touch.snap.touchHeld) {
      draggingSlider = false;
      if (haDragRow >= 0) {
        const auto& entry = ha::config().sheet[haLights[haDragRow]];
        haSend(ha::brightnessCall(entry.entity, haLevel[haDragRow]));
        haDragRow = -1;
      }
      cleanRefreshPending = true;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    close();
    return;
  }
  if (!Frontlight.present()) return;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!slideSwitchOwnsLight()) toggleLight();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left},
                                       [this] { adjustBrightness(-BRIGHTNESS_STEP); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right},
                                       [this] { adjustBrightness(BRIGHTNESS_STEP); });
}

int FrontlightPanelActivity::computePanelBottom() const {
  const auto tokens = uiThemeTokens(uiTarget);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int16_t lineHeight = uiTarget.lineHeight(tokens.smallText.font);
  // Slim battery band + the air around it (mirrors buildPanelScreen).
  const int y0 = std::max<int>(metrics.batteryHeight, lineHeight);
  int y = tokens.spaceMd + y0 + tokens.spaceMd;
  if (Frontlight.present()) {
    // Screen::sliderRow reserves caption + spaceMd + control band, then a
    // spaceMd gap; addSliderRow() adds one more spaceMd of air after each row.
    y += lineHeight + tokens.spaceMd + kSliderRowHeight + 2 * tokens.spaceMd;  // brightness
    if (Frontlight.hasColorTemperature()) {
      y += lineHeight + tokens.spaceMd + kSliderRowHeight + 2 * tokens.spaceMd;  // warmth
    }
    y += tokens.spaceSm;
  }
  y += haRowsHeight(lineHeight, tokens.spaceSm, tokens.spaceMd);
  // Icons are touch targets, so a buttons-only board gets no row and the
  // sheet is exactly the frontlight controls.
  if (mappedInput.hasTouch()) y += kQuickControlSize;
  // The sheet's grabber band: content margin + grabber + air to the edge.
  // buildPanelScreen() feeds the same theme spacings into SheetProps.
  y += 2 * (tokens.spaceLg + tokens.spaceMd) + kGrabberHeight;
  return y;
}

void FrontlightPanelActivity::panelScreen(UiScreen& screen, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->buildPanelScreen(screen);
}

void FrontlightPanelActivity::addSliderRow(UiScreen& screen, const char* label, const uint8_t value,
                                           const fui::ActionId sliderAction, const fui::ActionId stepAction,
                                           const bool showToggle) {
  // Live percentage readout. The row draws before this call returns
  // (immediate mode), so borrowing a stack buffer is safe.
  char pct[8];
  snprintf(pct, sizeof(pct), "%u%%", static_cast<unsigned>(value));

  // rowProps is a member (fui::SliderRowProps embeds a 324-byte StyleSet, well
  // past the 256-byte budget a local gets — AGENTS.md). Every field that
  // varies between the two rows is reassigned here; the rest keep their
  // constructed defaults, which already match the panel's card language.
  rowProps.label = label;
  rowProps.value = pct;
  rowProps.sliderValue = value;
  rowProps.sliderAction = sliderAction;
  rowProps.decrement = stepAction;
  rowProps.increment = stepAction;
  rowProps.decrementValue = -BRIGHTNESS_STEP;
  rowProps.incrementValue = BRIGHTNESS_STEP;
  if (showToggle) {
    // Lamp on/off after the +: the sliders set the level, this kills the light
    // outright. Filled glyph = on, outline = off.
    rowProps.toggleAction = ACTION_TOGGLE;
    rowProps.toggleIcon = fui::bitmapFromIcon(lightOn ? icon_sun_filled_32 : icon_sun_32);
  } else {
    rowProps.toggleAction = fui::NO_ACTION;
    rowProps.toggleIcon = fui::BitmapRef{};
  }
  screen.sliderRow(rowProps, kSliderRowHeight);
  // The wrapper's own trailing gap is one spaceMd; double it so the rows
  // breathe — a control band this tall reads cramped at the list cadence.
  screen.spacer(screen.theme().spaceMd);
}

void FrontlightPanelActivity::buildPanelScreen(UiScreen& screen) {
  const auto& theme = screen.theme();

  // Sheet chrome first: the card body, the 2px rule along its bottom edge, and
  // the grabber on the edge the sheet is dragged from. Screen::sheet() also
  // clamps the content area to the sheet, so every band below lays out inside
  // it. (No header: the panel is a floating card, and its own grabber says
  // what it is.)
  fui::SheetProps sheetProps;
  // Keep equal padding above and below the grabber.
  sheetProps.grabberMargin = static_cast<int16_t>(theme.spaceLg + theme.spaceMd);
  sheetProps.grabberInset = sheetProps.grabberMargin;
  screen.sheet(sheetProps, static_cast<int16_t>(panelBottom));
  screen.insetContent(fui::Insets{0, kPanelSideMargin, 0, kPanelSideMargin});

  // Reuse the exact battery renderer and header rectangle used by Home. Call
  // the base implementation directly because RoundedRaff suppresses its
  // untitled Home header.
  {
    const auto& metrics = UITheme::getInstance().getMetrics();
    screen.spacer(theme.spaceMd);
    const int16_t bandH = std::max<int16_t>(static_cast<int16_t>(metrics.batteryHeight),
                                            screen.target().lineHeight(theme.smallText.font));
    screen.takeTop(bandH, theme.spaceMd);
    UITheme::getInstance().getTheme().BaseTheme::drawHeader(
        renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.homeTopPadding - metrics.topPadding},
        nullptr);
  }

  if (Frontlight.present()) {
    addSliderRow(screen, tr(STR_BRIGHTNESS), brightness, ACTION_BRIGHTNESS, ACTION_BRIGHTNESS_STEP,
                 /*showToggle=*/!slideSwitchOwnsLight());
    if (Frontlight.hasColorTemperature()) {
      addSliderRow(screen, tr(STR_WARMTH), warmth, ACTION_WARMTH, ACTION_WARMTH_STEP, /*showToggle=*/false);
    }
    screen.spacer(theme.spaceSm);
  }

  addHaRows(screen);

  // Match the reader toolbar's equal icon slots; the full slot is the touch target.
  if (mappedInput.hasTouch()) {
    const int tileCount = bleturner::status().compiledIn ? kTileCount : kTileCount - 1;
    const fui::BitmapRef icons[kTileCount] = {
        fui::bitmapFromIcon(icon_moon_32), fui::bitmapFromIcon(icon_refresh_cw_32),
        fui::bitmapFromIcon(icon_reader_text_24), fui::bitmapFromIcon(icon_pointer_32),
        fui::bitmapFromIcon(SETTINGS.ble.enabled ? icon_bluetooth_32 : icon_bluetooth_off_32)};
    const bool enabled[kTileCount] = {SETTINGS.screenInverted != 0, false, false,
                                      SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_OFF,
                                      SETTINGS.ble.enabled != 0};
    static constexpr fui::Rotation rotations[] = {fui::Rotation::None, fui::Rotation::CCW90, fui::Rotation::R180,
                                                  fui::Rotation::CW90};
    // The underline rotates around the same 32px square as the toolbar's T.
    static constexpr fui::Rect underlines[] = {{4, 29, 24, 3}, {29, 4, 3, 24}, {4, 0, 24, 3}, {0, 4, 3, 24}};
    // The panel inherits the current screen rotation until the reader resumes.
    constexpr int orientationCount = CrossPointSettings::ORIENTATION_COUNT;
    const int preview = (SETTINGS.orientation + orientationCount - renderer.getOrientation()) % orientationCount;
    const fui::Rect row = screen.takeTop(kQuickControlSize);
    const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
    for (int id = 0; id < tileCount; ++id) {
      const int16_t left = static_cast<int16_t>(row.x + row.width * id / tileCount);
      const int16_t right = static_cast<int16_t>(row.x + row.width * (id + 1) / tileCount);
      const fui::Rect slot{left, row.y, static_cast<int16_t>(right - left), row.height};
      const fui::Rect control{static_cast<int16_t>(slot.x + (slot.width - kQuickControlSize) / 2), row.y,
                              kQuickControlSize, kQuickControlSize};
      if (enabled[id]) {
        screen.target().fill(control, ink, theme.controlRadius);
      } else {
        screen.target().stroke(control, ink, 2, theme.controlRadius);
      }
      const fui::Rect iconRect{static_cast<int16_t>(control.x + (control.width - kQuickIconSize) / 2),
                               static_cast<int16_t>(control.y + (control.height - kQuickIconSize) / 2), kQuickIconSize,
                               kQuickIconSize};
      const fui::Paint foreground = fui::Paint::solid(enabled[id] ? fui::Color::White : fui::Color::Black);
      screen.target().bitmap(iconRect, icons[id], fui::BitmapMode::Center, foreground,
                             id == 2 ? rotations[preview] : fui::Rotation::None);
      if (id == 2) {
        const auto& line = underlines[preview];
        screen.target().fill(fui::Rect{static_cast<int16_t>(iconRect.x + line.x),
                                       static_cast<int16_t>(iconRect.y + line.y), line.width, line.height},
                             ink);
      }
      // A tile whose function the slide switch owns shows its state but takes no tap.
      const bool switchOwned = (id == 0 && slideSwitchOwns(CrossPointSettings::SLIDE_SWITCH_NIGHT_MODE)) ||
                               (id == 3 && slideSwitchOwns(CrossPointSettings::SLIDE_SWITCH_TOUCH_CONTROLS));
      if (!switchOwned) screen.frame().hit(slot, ACTION_TILE, static_cast<int16_t>(id), fui::InputTouch);
    }
  }
}

void FrontlightPanelActivity::render(RenderLock&&) {
  panelBottom = computePanelBottom();

  // fui::sheet draws the card body, its bottom rule, and the grabber during
  // renderUi(); the battery band at the card's top is part of the screen build.
  renderUi();
  fadeHaRows();

  // A tile that rewrote the whole frame (night mode) re-drives every pixel
  // once; ordinary repaints stay on the fast path. HALF: strong enough to
  // flip the whole frame's polarity without the FULL waveform's blackout
  // flash. Any faint residue clears with the panel's dedicated refresh tile
  // or the next scheduled clean refresh.
  renderer.displayBuffer(cleanRefreshPending ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  cleanRefreshPending = false;
}

// --- Home Assistant rows -----------------------------------------------------

void FrontlightPanelActivity::loadHaRows() {
  haLightCount = haButtonCount = 0;
  if (!ha::enabled() || !mappedInput.hasTouch()) return;
  const auto& sheet = ha::config().sheet;
  for (int i = 0; i < static_cast<int>(sheet.size()); ++i) {
    if (sheet[i].kind == ha::SheetKind::Light) {
      if (haLightCount < kMaxHaLights) haLights[haLightCount++] = i;
    } else if (haButtonCount < static_cast<int>(ha::kMaxSheetButtons)) {
      haButtons[haButtonCount++] = i;
    }
  }
  const auto& cache = ha::stateCache();
  for (int r = 0; r < haLightCount; ++r) {
    const auto it = cache.find(sheet[haLights[r]].entity);
    haLevel[r] = it != cache.end() && it->second.brightnessPct > 0 ? static_cast<uint8_t>(it->second.brightnessPct) : 0;
  }
  haShownStatus = haLink.status();
}

int FrontlightPanelActivity::haRowsHeight(const int16_t lineHeight, const int16_t /*spaceSm*/,
                                          const int16_t spaceMd) const {
  if (haLightCount == 0 && haButtonCount == 0) return 0;
  int y = lineHeight + spaceMd;  // section title, or why HA is unreachable
  y += haLightCount * (lineHeight + spaceMd + kSliderRowHeight + 2 * spaceMd);
  if (haButtonCount > 0) y += kQuickControlSize + spaceMd;
  return y;
}

bool FrontlightPanelActivity::haUnreachable() const {
  return haShownStatus == ha::LinkStatus::NoNetwork || haShownStatus == ha::LinkStatus::NoAnswer ||
         haShownStatus == ha::LinkStatus::TokenRejected;
}

void FrontlightPanelActivity::addHaRows(UiScreen& screen) {
  if (haLightCount == 0 && haButtonCount == 0) {
    haTop = haBottom = 0;
    return;
  }
  const auto& theme = screen.theme();
  const auto& sheet = ha::config().sheet;
  const auto& cache = ha::stateCache();
  const auto nameOf = [&](const ha::SheetEntry& e) -> const char* {
    if (!e.name.empty()) return e.name.c_str();
    const auto it = cache.find(e.entity);
    return it != cache.end() && !it->second.name.empty() ? it->second.name.c_str() : e.entity.c_str();
  };

  const char* title = tr(STR_HA_TITLE);
  if (haShownStatus == ha::LinkStatus::NoNetwork) title = tr(STR_HA_NO_NETWORK);
  if (haShownStatus == ha::LinkStatus::NoAnswer) title = tr(STR_HA_NO_ANSWER);
  if (haShownStatus == ha::LinkStatus::TokenRejected) title = tr(STR_HA_TOKEN_REJECTED);
  const fui::Rect titleRow = screen.takeTop(screen.target().lineHeight(theme.smallText.font), theme.spaceMd);
  screen.target().text(titleRow, title, theme.smallText);
  // The fade covers the controls, not the line saying why.
  haTop = static_cast<int16_t>(titleRow.y + titleRow.height + theme.spaceMd);

  for (int r = 0; r < haLightCount; ++r) {
    const auto& entry = sheet[haLights[r]];
    const auto it = cache.find(entry.entity);
    const bool seen = it != cache.end();  // never seen: no value until the first call
    const bool known =
        seen && !it->second.state.empty() && it->second.state != "unavailable" && it->second.state != "unknown";
    const bool on = known && it->second.state == "on";
    char value[16];
    if (entry.entity == haPendingEntity) {
      snprintf(value, sizeof(value), "...");
    } else if (!seen) {
      value[0] = '\0';
    } else if (!known) {
      snprintf(value, sizeof(value), "%s", tr(STR_HA_UNAVAILABLE));
    } else if (on) {
      snprintf(value, sizeof(value), "%u%%", static_cast<unsigned>(haLevel[r]));
    } else {
      snprintf(value, sizeof(value), "%s", tr(STR_STATE_OFF));
    }
    rowProps.label = nameOf(entry);
    rowProps.value = value;
    rowProps.sliderValue = on || haDragRow == r ? haLevel[r] : 0;
    rowProps.sliderAction = static_cast<fui::ActionId>(ACTION_HA_SLIDER + r);
    rowProps.decrement = ACTION_HA_DOWN;
    rowProps.increment = ACTION_HA_UP;
    rowProps.decrementValue = rowProps.incrementValue = static_cast<int16_t>(r);
    rowProps.toggleAction = ACTION_HA_TOGGLE;
    rowProps.toggleValue = static_cast<int16_t>(r);
    rowProps.toggleIcon = fui::bitmapFromIcon(on ? icon_sun_filled_32 : icon_sun_32);
    screen.sliderRow(rowProps, kSliderRowHeight);
    screen.spacer(theme.spaceMd);
  }

  if (haButtonCount > 0) {
    const fui::Rect row = screen.takeTop(kQuickControlSize, theme.spaceMd);
    const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
    for (int b = 0; b < haButtonCount; ++b) {
      const auto& entry = sheet[haButtons[b]];
      const auto it = cache.find(entry.entity);
      const bool on = it != cache.end() && it->second.state == "on";
      const int16_t left = static_cast<int16_t>(row.x + row.width * b / haButtonCount);
      const int16_t right = static_cast<int16_t>(row.x + row.width * (b + 1) / haButtonCount);
      const fui::Rect slot{static_cast<int16_t>(left + theme.spaceSm / 2), row.y,
                           static_cast<int16_t>(right - left - theme.spaceSm), row.height};
      if (on) {
        screen.target().fill(slot, ink, theme.controlRadius);
      } else {
        screen.target().stroke(slot, ink, 2, theme.controlRadius);
      }
      fui::TextStyle label = theme.smallText;
      label.align = fui::TextAlign::Center;
      label.maxLines = 2;
      label.color = on ? fui::Color::White : fui::Color::Black;
      const char* text = entry.entity == haPendingEntity ? "..." : nameOf(entry);
      const fui::Rect textRect{
          static_cast<int16_t>(slot.x + 4),
          static_cast<int16_t>(slot.y + (slot.height - screen.target().lineHeight(label.font)) / 2),
          static_cast<int16_t>(slot.width - 8), screen.target().lineHeight(label.font)};
      screen.target().text(textRect, text, label);
      screen.frame().hit(slot, ACTION_HA_BUTTON, static_cast<int16_t>(b), fui::InputTouch);
    }
  }
  haBottom = screen.contentRect().y;
}

void FrontlightPanelActivity::fadeHaRows() {
  if (!haUnreachable() || haBottom <= haTop) return;
  // Every other pixel white: the rows read as grayed out but stay legible.
  const int width = renderer.getScreenWidth();
  for (int y = haTop; y < haBottom; ++y) {
    for (int x = (y & 1); x < width; x += 2) renderer.drawPixel(x, y, false);
  }
}

void FrontlightPanelActivity::haSend(const ha::ServiceCall& call) {
  if (haLink.submit(call)) {
    haPendingEntity = call.entityId;
    haHasQueued = false;
  } else {
    haQueued = call;
    haHasQueued = true;
  }
  haShownStatus = haLink.status();
  requestUpdate();
}

void FrontlightPanelActivity::pollHa() {
  if (haLightCount == 0 && haButtonCount == 0) return;
  bool changed = false;
  if (!haPendingEntity.empty() && !haLink.busy()) {
    std::string entity;
    ha::EntityState fresh;
    if (haLink.takeResult(entity, fresh)) {
      ha::updateCachedState(entity, fresh);
      ha::saveStateCacheIfDirty();
      for (int r = 0; r < haLightCount; ++r) {
        if (ha::config().sheet[haLights[r]].entity == entity && fresh.brightnessPct > 0) {
          haLevel[r] = static_cast<uint8_t>(fresh.brightnessPct);
        }
      }
    }
    haPendingEntity.clear();
    changed = true;
  }
  if (haHasQueued && !haLink.busy()) {
    haSend(haQueued);
    return;
  }
  const ha::LinkStatus status = haLink.status();
  if (status != haShownStatus && status != ha::LinkStatus::Working) {
    haShownStatus = status;
    changed = true;
  }
  if (changed) requestUpdate();
}

void FrontlightPanelActivity::onHaSliderEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  const int row = event.action - ACTION_HA_SLIDER;
  if (row < 0 || row >= self->haLightCount) return;
  self->haDragRow = row;
  self->haLevel[row] = percentFromPermille(event.dragPermille);
}

void FrontlightPanelActivity::onHaStepEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  const int row = event.value;
  if (row < 0 || row >= self->haLightCount) return;
  const int step = event.action == ACTION_HA_UP ? HA_BRIGHTNESS_STEP : -HA_BRIGHTNESS_STEP;
  const int next = std::max(0, std::min(100, static_cast<int>(self->haLevel[row]) + step));
  self->haLevel[row] = static_cast<uint8_t>(next);
  self->haSend(ha::brightnessCall(ha::config().sheet[self->haLights[row]].entity, next));
}

void FrontlightPanelActivity::onHaToggleEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.value < 0 || event.value >= self->haLightCount) return;
  ha::ServiceCall call;
  if (ha::tapAction(ha::config().sheet[self->haLights[event.value]].entity, call)) self->haSend(call);
}

void FrontlightPanelActivity::onHaButtonEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.value < 0 || event.value >= self->haButtonCount) return;
  ha::ServiceCall call;
  if (ha::tapAction(ha::config().sheet[self->haButtons[event.value]].entity, call)) self->haSend(call);
}
