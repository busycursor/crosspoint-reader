#pragma once

#include <HaDashboard.h>
#include <HaIdlePolicy.h>
#include <HaState.h>

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/UiAppHost.h"

class HaSession;

// Home > Control: one Home Assistant dashboard (named in /homeassistant.json)
// drawn in black and white. It draws the cached layout and states at once,
// then joins the home network and updates cards in place while open.
class ControlActivity final : public Activity, private UiAppHost {
  // One drawable line of the page: a section title or a card.
  struct Item {
    int section;
    int card;  // -1: the section's title
    int16_t height;
  };

  std::shared_ptr<HaSession> session;
  ha::Dashboard dash;
  std::vector<Item> items;
  std::vector<int> pageStarts;  // index into items
  int page = 0;

  std::string pendingEntity;  // a tap in flight: drawn as working
  ha::ServiceCall queued;
  bool hasQueued = false;
  int dragCard = -1;  // index into items of the slider being dragged
  uint8_t dragLevel = 0;
  bool draggingSlider = false;
  ha::LinkStatus shownStatus = ha::LinkStatus::Working;
  bool repaintDue = false;     // session changes waiting for the rate limit
  unsigned long lastPaintMs = 0;

  freeink::ui::SliderRowProps rowProps;

  static void screenBuilder(UiScreen& screen, void* user);
  static void onCardEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onSliderEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onStepEvent(const freeink::ui::ActionEvent& event, void* user);

  void loadCachedLayout();
  void setLayout(const std::string& json, bool save);
  void paginate(int16_t available);
  void buildScreen(UiScreen& screen);
  void drawCard(UiScreen& screen, const ha::Card& card, int itemIndex);
  void send(const ha::ServiceCall& call);
  void pollLink();
  void pollSession();
  const ha::Card* cardAt(int itemIndex) const;

 public:
  ControlActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool allowsHomeAssistantWifiOff() const override { return true; }
};
