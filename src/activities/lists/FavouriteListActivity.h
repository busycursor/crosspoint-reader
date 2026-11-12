#pragma once

#include <cstdint>
#include <vector>

#include "activities/UiListActivity.h"

// Settings -> Favourite list: "Starred in Vikunja" (the default: the first
// starred list) plus the saved Vikunja projects to pick another. The choice
// is stored by project id, so it survives lists being added or reordered.
// Lists opens straight into the favourite.
class FavouriteListActivity final : public UiListActivity {
 public:
  FavouriteListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);

  std::vector<int64_t> choices;  // 0: starred in Vikunja
  char rowBuf[160] = {};
};
