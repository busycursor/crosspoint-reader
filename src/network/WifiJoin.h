#pragma once

#include <string>

// Joins the most recently used saved network without any UI. Returns true
// once connected (immediately when already connected), false on timeout or
// when no network is saved. Blocks the caller for up to timeoutMs.
bool joinSavedWifi(unsigned long timeoutMs);
// Joins one saved network by SSID, without scanning. True once connected to
// it (immediately when already connected to it); false when it is not saved,
// on timeout, or while connected to a different network (left alone).
bool joinWifiNetwork(const std::string& ssid, unsigned long timeoutMs);
