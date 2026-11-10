#include "HaStateCache.h"

#include <HalStorage.h>
#include <Logging.h>

namespace ha {
namespace {

constexpr size_t kMaxCacheBytes = 32 * 1024;

StateMap cache;
bool loaded = false;
bool dirty = false;

bool sameState(const EntityState& a, const EntityState& b) {
  return a.state == b.state && a.name == b.name && a.unit == b.unit && a.brightnessPct == b.brightnessPct &&
         a.forecast == b.forecast && a.temperature == b.temperature;
}

}  // namespace

StateMap& stateCache() {
  if (!loaded) {
    loaded = true;
    std::string json;
    if (Storage.exists(kStateCachePath) && Storage.readFileToString("HA", kStateCachePath, kMaxCacheBytes, json) &&
        !parseStates(json, cache)) {
      LOG_ERR("HA", "HA state cache unreadable; starting empty");
      cache.clear();
    }
  }
  return cache;
}

bool updateCachedState(const std::string& entityId, const EntityState& s) {
  auto& m = stateCache();
  auto it = m.find(entityId);
  EntityState next = s;
  // A forecast arrives separately from the state; keep the last one.
  if (it != m.end() && next.forecast.empty()) next.forecast = it->second.forecast;
  if (it != m.end() && sameState(it->second, next)) return false;
  m[entityId] = std::move(next);
  dirty = true;
  return true;
}

void saveStateCacheIfDirty() {
  if (!dirty) return;
  Storage.ensureDirectoryExists("/.crosspoint");
  if (Storage.writeFile(kStateCachePath, String(serializeStates(stateCache()).c_str()))) {
    dirty = false;
  } else {
    LOG_ERR("HA", "HA state cache: write failed");
  }
}

}  // namespace ha
