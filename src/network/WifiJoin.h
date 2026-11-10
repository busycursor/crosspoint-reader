#pragma once

// Joins the most recently used saved network without any UI. Returns true
// once connected (immediately when already connected), false on timeout or
// when no network is saved. Blocks the caller for up to timeoutMs.
bool joinSavedWifi(unsigned long timeoutMs);
