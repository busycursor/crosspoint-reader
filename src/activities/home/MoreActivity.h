#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Home's "More" submenu: Browse Files and File Transfer, drawn like the home
// menu. Back returns home with More selected.
class MoreActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;

  void activate(int index);
  int menuTop() const;

 public:
  MoreActivity(GfxRenderer& renderer, MappedInputManager& mappedInput) : Activity("More", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
