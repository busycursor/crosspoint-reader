#include "WaveformActions.h"

namespace waveform_actions {
namespace {

inline uint8_t replicate(const uint8_t code, const int bit) { return ((code >> bit) & 1) ? 0xFF : 0x00; }

inline uint32_t popcount8(uint8_t v) {
  uint32_t n = 0;
  for (; v; v &= static_cast<uint8_t>(v - 1)) ++n;
  return n;
}

}  // namespace

ActionCounts computeActionPlanes(const uint8_t* prevLsb, const uint8_t* prevMsb, const uint8_t* newLsb,
                                 const uint8_t* newMsb, const size_t bytes, const ActionCodes& codes, uint8_t* outA,
                                 uint8_t* outB) {
  ActionCounts counts;
  const uint8_t holdA = replicate(codes.hold, 0), holdB = replicate(codes.hold, 1);
  const uint8_t whiteA = replicate(codes.toWhite, 0), whiteB = replicate(codes.toWhite, 1);
  const uint8_t blackA = replicate(codes.toBlack, 0), blackB = replicate(codes.toBlack, 1);
  const uint8_t grayA = replicate(codes.toGray, 0), grayB = replicate(codes.toGray, 1);
  for (size_t i = 0; i < bytes; ++i) {
    const uint8_t pl = prevLsb[i], pm = prevMsb[i], nl = newLsb[i], nm = newMsb[i];
    const uint8_t pWhite = pl & pm, pBlack = static_cast<uint8_t>(~(pl | pm)), pGray = pl ^ pm;
    const uint8_t nWhite = nl & nm, nBlack = static_cast<uint8_t>(~(nl | nm)), nGray = nl ^ nm;
    const uint8_t toWhite = nWhite & static_cast<uint8_t>(~pWhite);
    const uint8_t toBlack = nBlack & static_cast<uint8_t>(~pBlack);
    const uint8_t toGray = nGray & static_cast<uint8_t>(~pGray);
    const uint8_t hold = static_cast<uint8_t>(~(toWhite | toBlack | toGray));
    outA[i] = static_cast<uint8_t>((hold & holdA) | (toWhite & whiteA) | (toBlack & blackA) | (toGray & grayA));
    outB[i] = static_cast<uint8_t>((hold & holdB) | (toWhite & whiteB) | (toBlack & blackB) | (toGray & grayB));
    counts.hold += popcount8(hold);
    counts.toWhite += popcount8(toWhite);
    counts.toBlack += popcount8(toBlack);
    counts.toGray += popcount8(toGray);
    counts.grayFromBlack += popcount8(toGray & pBlack);
  }
  return counts;
}

void tonesFromOverlay(const uint8_t* bw, const uint8_t* grayMask, const size_t bytes, uint8_t* outLsb,
                      uint8_t* outMsb) {
  for (size_t i = 0; i < bytes; ++i) {
    const uint8_t gray = grayMask[i];
    const uint8_t white = bw[i] & static_cast<uint8_t>(~gray);
    outLsb[i] = white | gray;  // white (1,1), gray (1,0), black (0,0)
    outMsb[i] = white;
  }
}

}  // namespace waveform_actions
