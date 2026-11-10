#pragma once

// Single-pass anti-aliasing (Settings > Reader > Refresh: Single, UC8279):
// remembers the tones the reader last put on the panel, so the next page
// drives only the pixels that change. See lib/WaveformActions.

#include <WaveformActions.h>

#include <cstddef>
#include <cstdint>

class GfxRenderer;

namespace gray_tracker {

// True when this panel can run single-pass pages (UC8279 with Direct actions;
// never in the simulator, whose display has no waveform tables).
bool panelSupported();
// Marks the Direct pass about to be sent as an action page (or ends it).
void setPanelActions(bool enabled);

// Plane codes for the four actions (calibrated defaults; the dev API can
// override them for waveform experiments).
void setCodes(const waveform_actions::ActionCodes& codes);
const waveform_actions::ActionCodes& codes();
// True when the panel still shows the tones recorded last: nothing else was
// displayed since (judged by the renderer's frame count).
bool previousKnown(const GfxRenderer& renderer);
void invalidate();

// Stock two-step page: keep the MSB overlay mask (right after rendering it),
// then record the page's tones from the restored B/W frame once it is shown.
void captureOverlayMask(const uint8_t* fb, size_t bytes);
void recordOverlayPage(const GfxRenderer& renderer, const uint8_t* bwFb, size_t bytes);

// Single-pass page: stash the LSB tone plane (right after rendering it), then
// turn LSB + MSB into the two action planes. The new tones become the
// recorded ones; call markShown() once the page is on the panel.
bool stashNewLsb(const uint8_t* fb, size_t bytes);
bool makeActionPlanes(const uint8_t* msbFb, size_t bytes, const uint8_t** planeA, const uint8_t** planeB,
                      waveform_actions::ActionCounts* counts);
void markShown(const GfxRenderer& renderer);

}  // namespace gray_tracker
