#include "WaveformActions.h"

#include <gtest/gtest.h>

using namespace waveform_actions;

namespace {

enum Tone { W, B, G };

// One byte per test: 8 pixels, tones given MSB-first like the panel planes.
void planes(const Tone (&t)[8], uint8_t& lsb, uint8_t& msb) {
  lsb = msb = 0;
  for (int i = 0; i < 8; ++i) {
    const uint8_t bit = static_cast<uint8_t>(0x80 >> i);
    if (t[i] == W) {
      lsb |= bit;
      msb |= bit;
    } else if (t[i] == G) {
      lsb |= bit;  // (1, 0); (0, 1) is gray too
    }
  }
}

int codeAt(uint8_t a, uint8_t b, int i) {
  const uint8_t bit = static_cast<uint8_t>(0x80 >> i);
  return ((a & bit) ? 1 : 0) | ((b & bit) ? 2 : 0);
}

}  // namespace

TEST(WaveformActions, UnchangedPixelsHold) {
  const Tone same[8] = {W, W, B, B, G, G, W, B};
  uint8_t pl, pm, nl, nm, a, b;
  planes(same, pl, pm);
  planes(same, nl, nm);
  const ActionCodes codes;  // hold=0
  const auto counts = computeActionPlanes(&pl, &pm, &nl, &nm, 1, codes, &a, &b);
  EXPECT_EQ(counts.hold, 8u);
  for (int i = 0; i < 8; ++i) EXPECT_EQ(codeAt(a, b, i), codes.hold);
}

TEST(WaveformActions, EachTransitionGetsItsCode) {
  const Tone prev[8] = {B, G, W, G, W, B, W, B};
  const Tone next[8] = {W, W, B, B, G, G, W, B};
  uint8_t pl, pm, nl, nm, a, b;
  planes(prev, pl, pm);
  planes(next, nl, nm);
  const ActionCodes codes{0, 1, 2, 3};
  const auto counts = computeActionPlanes(&pl, &pm, &nl, &nm, 1, codes, &a, &b);
  const int expected[8] = {1, 1, 2, 2, 3, 3, 0, 0};
  for (int i = 0; i < 8; ++i) EXPECT_EQ(codeAt(a, b, i), expected[i]) << "pixel " << i;
  EXPECT_EQ(counts.toWhite, 2u);
  EXPECT_EQ(counts.toBlack, 2u);
  EXPECT_EQ(counts.toGray, 2u);
  EXPECT_EQ(counts.hold, 2u);
  EXPECT_EQ(counts.grayFromBlack, 1u);
}

TEST(WaveformActions, BothGrayEncodingsAreGray) {
  // (1,0) and (0,1) are the same class: gray -> other gray holds.
  const uint8_t pl = 0xFF, pm = 0x00, nl = 0x00, nm = 0xFF;
  uint8_t a, b;
  const auto counts = computeActionPlanes(&pl, &pm, &nl, &nm, 1, ActionCodes{}, &a, &b);
  EXPECT_EQ(counts.hold, 8u);
}

TEST(WaveformActions, CodesAreRemappable) {
  // Calibration may put "to black" on code 0; the planes must follow.
  const Tone prev[8] = {W, W, W, W, W, W, W, W};
  const Tone next[8] = {B, B, B, B, W, W, W, W};
  uint8_t pl, pm, nl, nm, a, b;
  planes(prev, pl, pm);
  planes(next, nl, nm);
  const ActionCodes codes{3, 1, 0, 2};
  computeActionPlanes(&pl, &pm, &nl, &nm, 1, codes, &a, &b);
  EXPECT_EQ(a, 0x0F);  // black pixels code 0 -> A=0; held pixels code 3 -> A=1
  EXPECT_EQ(b, 0x0F);
}

TEST(WaveformActions, TonesFromOverlayMarksGrayAndKeepsBase) {
  const uint8_t bw = 0xF0, mask = 0x3C;  // pixels 2..5 gray; 0,1 white; 6,7 black
  uint8_t l, m;
  tonesFromOverlay(&bw, &mask, 1, &l, &m);
  EXPECT_EQ(l, 0xFC);  // white or gray
  EXPECT_EQ(m, 0xC0);  // white only
}
