#pragma once

#include "components/themes/lyra/LyraTheme.h"

// Lyra with a home cover a third shorter, leaving room for more menu rows.
namespace LyraCompactMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = LyraMetrics::values;
  v.homeCoverHeight = 150;
  v.homeCoverTileHeight = 166;  // cover plus the selection padding above and below
  return v;
}();
}  // namespace LyraCompactMetrics

class LyraCompactTheme : public LyraTheme {
 public:
  LyraCompactTheme() : LyraTheme(LyraCompactMetrics::values.homeCoverHeight) {}
};
