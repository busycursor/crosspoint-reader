#pragma once

#include <cstddef>
#include <cstdint>

// Single-pass grayscale experiment (see src/dev/DevApi.h): instead of the
// target tone, each pixel's two controller plane bits carry an ACTION chosen
// from its previous and new tone, so unchanged pixels idle (as in stock B/W
// partial refreshes) and only changing pixels are driven.
//
// Tones use absolute planes: white = (1, 1), black = (0, 0), and any mixed
// pair is gray (stock AA drives both gray levels with the same pulse).
namespace waveform_actions {

// 2-bit plane codes for each action: bit 0 goes to plane A, bit 1 to plane B.
// Which code selects which LUT register is found by calibration.
struct ActionCodes {
  uint8_t hold = 0;
  uint8_t toWhite = 1;
  uint8_t toBlack = 2;
  uint8_t toGray = 3;
};

struct ActionCounts {
  uint32_t hold = 0;
  uint32_t toWhite = 0;
  uint32_t toBlack = 0;
  uint32_t toGray = 0;
  uint32_t grayFromBlack = 0;  // gray targets whose previous tone was black (longest drive)
};

// Computes the action planes for `bytes` bytes of 1-bit planes (8 pixels per
// byte). prev* and new* are absolute tone planes; outA/outB may alias newLsb
// and newMsb respectively only if the caller no longer needs them.
ActionCounts computeActionPlanes(const uint8_t* prevLsb, const uint8_t* prevMsb, const uint8_t* newLsb,
                                 const uint8_t* newMsb, size_t bytes, const ActionCodes& codes, uint8_t* outA,
                                 uint8_t* outB);

// Builds absolute tone planes from a B/W frame (1 = white) and an overlay
// gray mask (1 = gray, as the MSB overlay mask marks every gray pixel), for
// recording what a stock two-step page left on the panel. Gray is (1, 0).
void tonesFromOverlay(const uint8_t* bw, const uint8_t* grayMask, size_t bytes, uint8_t* outLsb, uint8_t* outMsb);

}  // namespace waveform_actions
