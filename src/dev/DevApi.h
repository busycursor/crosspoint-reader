#pragma once

// Development control API. A host tool reads the screen, logs, state and
// settings, drives screens and input, controls the frontlight and power,
// edits SD files, sets Wi-Fi and flashes firmware. Two transports share one
// set of endpoints: HTTP on port 8090 (bearer token) and USB serial (framed
// lines, no token). Built only with -DCROSSPOINT_DEV_API (the x4pro-dev and
// simulator environments); release builds compile none of it.
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

#else

inline void begin(void (*)()) {}
inline void loop() {}
inline void applyPending() {}
inline bool keepAwake() { return false; }

#endif

}  // namespace dev_api
