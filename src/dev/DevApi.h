#pragma once

// Development control API, built into every firmware we make and switched at
// runtime by Settings > Connectivity > Developer Mode. A host tool reads the screen, logs, state and
// settings, drives screens and input, controls the frontlight and power,
// edits SD files, sets Wi-Fi and flashes firmware. Two transports share one
// set of endpoints: HTTP on port 8090 (bearer token) and USB serial (framed
// lines, no token). With Developer Mode off only the serial transport answers
// (so a cable can turn the mode back on); Wi-Fi, the HTTP server and the
// sleep hold are all off. Builds without -DCROSSPOINT_DEV_API compile none of it.
//
// Handlers never act on app state directly: they store one pending command
// that main.cpp applies at a fixed point in loop(), the same place real input
// is handled.
namespace dev_api {

#ifdef CROSSPOINT_DEV_API

// enterSleep puts the device into deep sleep (main.cpp owns that sequence).
void begin(void (*enterSleep)());
// Serve requests and keep Wi-Fi up. Call once per loop().
void loop();
// Apply a queued open/input command. Call after mappedInputManager.update().
void applyPending();
// True while a host tool has been active recently; holds off auto-sleep.
bool keepAwake();
// Developer Mode is on: Wi-Fi stays up and network screens keep it on exit.
bool enabled();
// Waveform experiment: grayscale reader pages go out as one Direct-mode
// refresh with the experiment tables. Runtime only; a reboot clears it.
bool directGrayExperiment();
// True while a host tool is changing things (any non-read request, or an
// operation one started), and for a few seconds after. Drives the on-screen
// "API active" badge so the user knows not to use the device meanwhile.
bool indicatorActive();

#else

inline void begin(void (*)()) {}
inline void loop() {}
inline void applyPending() {}
inline bool keepAwake() { return false; }
inline bool enabled() { return false; }
inline bool directGrayExperiment() { return false; }
inline bool indicatorActive() { return false; }

#endif

}  // namespace dev_api
