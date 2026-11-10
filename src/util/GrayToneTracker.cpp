#include "GrayToneTracker.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>

#include <Logging.h>
#include <esp_heap_caps.h>

#include <cstring>

namespace gray_tracker {

bool panelSupported() {
#ifdef SIMULATOR
  return false;
#else
  return display.getController() == HalDisplay::Controller::UC8279 && display.supportsDirectActions();
#endif
}

void setPanelActions(const bool enabled) {
#ifdef SIMULATOR
  (void)enabled;
#else
  display.setDirectActions(enabled);
#endif
}

namespace {

waveform_actions::ActionCodes actionCodes;
bool known = false;
uint32_t shownFrame = 0;  // renderer frame count right after the recorded page
size_t planeBytes = 0;
// prevLsb, prevMsb, newLsb, scratch (overlay mask or new MSB), outA, outB
uint8_t* buf[6] = {};

bool ensureBuffers(const size_t bytes) {
  if (planeBytes == bytes && buf[0]) return true;
  for (auto*& b : buf) {
    heap_caps_free(b);
    b = nullptr;
  }
  for (auto*& b : buf) {
    b = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
    if (!b) {
      LOG_ERR("GRAY", "No PSRAM for single-pass planes");
      for (auto*& f : buf) {
        heap_caps_free(f);
        f = nullptr;
      }
      planeBytes = 0;
      known = false;
      return false;
    }
  }
  planeBytes = bytes;
  return true;
}

}  // namespace

void setCodes(const waveform_actions::ActionCodes& codes) { actionCodes = codes; }
const waveform_actions::ActionCodes& codes() { return actionCodes; }
bool previousKnown(const GfxRenderer& renderer) { return known && renderer.panelFrameCount() == shownFrame; }
void invalidate() { known = false; }

void captureOverlayMask(const uint8_t* fb, const size_t bytes) {
  if (ensureBuffers(bytes)) memcpy(buf[3], fb, bytes);
}

void recordOverlayPage(const GfxRenderer& renderer, const uint8_t* bwFb, const size_t bytes) {
  if (!ensureBuffers(bytes)) return;
  waveform_actions::tonesFromOverlay(bwFb, buf[3], bytes, buf[0], buf[1]);
  known = true;
  shownFrame = renderer.panelFrameCount();
}

void markShown(const GfxRenderer& renderer) {
  if (known) shownFrame = renderer.panelFrameCount();
}

bool stashNewLsb(const uint8_t* fb, const size_t bytes) {
  if (!ensureBuffers(bytes)) return false;
  memcpy(buf[2], fb, bytes);
  return true;
}

bool makeActionPlanes(const uint8_t* msbFb, const size_t bytes, const uint8_t** planeA, const uint8_t** planeB,
                      waveform_actions::ActionCounts* counts) {
  if (!known || planeBytes != bytes) return false;
  memcpy(buf[3], msbFb, bytes);
  const auto c = waveform_actions::computeActionPlanes(buf[0], buf[1], buf[2], buf[3], bytes, actionCodes, buf[4],
                                                       buf[5]);
  if (counts) *counts = c;
  // The new tones are what the panel will show next.
  memcpy(buf[0], buf[2], bytes);
  memcpy(buf[1], buf[3], bytes);
  *planeA = buf[4];
  *planeB = buf[5];
  return true;
}

}  // namespace gray_tracker

