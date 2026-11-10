#pragma once

#include <HaState.h>

// Last known Home Assistant states, kept on the SD card so the sheet and the
// Control page draw at once, before (or without) any network.
namespace ha {

constexpr const char* kStateCachePath = "/.crosspoint/homeassistant-state.json";

// Loaded from the SD card on first use.
StateMap& stateCache();
// Records a fresh state; true when it differs from the cached one.
bool updateCachedState(const std::string& entityId, const EntityState& s);
// Writes the cache when something changed since the last save.
void saveStateCacheIfDirty();

}  // namespace ha
