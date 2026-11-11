#include "MoreActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "HapticFeedback.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"

namespace {
constexpr int kItemCount = 2;  // Browse Files, File Transfer
}

void MoreActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

int MoreActivity::menuTop() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
}

void MoreActivity::activate(const int index) {
  activityManager.returnToMoreOnExit();
  if (index == 0) {
    activityManager.goToFileBrowser();
  } else {
    activityManager.goToFileTransfer();
  }
}

void MoreActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goHome(HomeMenuItem::MORE);
    return;
  }

  buttonNavigator.onNext([this] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, kItemCount);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, kItemCount);
    requestUpdate();
  });

  int row = -1;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int rowHeight = GUI.getMenuRowHeight(renderer);
  const auto touch =
      mappedInput.rowTouch(row, menuTop(), rowHeight + metrics.menuSpacing, kItemCount, 0, INT32_MAX, rowHeight);
  if (touch == MappedInputManager::RowTouch::Down) {
    if (selectorIndex != row) {
      selectorIndex = row;
      requestUpdate();
    }
    return;
  }
  if (touch == MappedInputManager::RowTouch::Tap) {
    selectorIndex = row;
    haptic_feedback::touchAction();
    activate(row);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activate(selectorIndex);
  }
}

void MoreActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const Rect content = UITheme::getInstance().getContentArea(renderer);

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_MORE));

  static constexpr UIIcon kIcons[kItemCount] = {UIIcon::Folder, UIIcon::Transfer};
  GUI.drawButtonMenu(
      renderer, Rect{content.x, menuTop(), content.width, pageHeight - menuTop() - metrics.buttonHintsHeight},
      kItemCount, selectorIndex,
      [](int index) { return std::string(index == 0 ? tr(STR_BROWSE_FILES) : tr(STR_FILE_TRANSFER)); },
      [](int index) { return kIcons[index]; });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
