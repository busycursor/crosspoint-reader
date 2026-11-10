#include "HaLive.h"

#include <ArduinoJson.h>

#include <cstdlib>

namespace ha {
namespace {

bool sameShown(const EntityState& a, const EntityState& b) {
  return a.state == b.state && a.name == b.name && a.unit == b.unit && a.brightnessPct == b.brightnessPct &&
         a.temperature == b.temperature;
}

// Rebuilds the derived fields after a patch: brightness only while on.
void applyAttributes(const std::string& entityId, JsonObjectConst attrs, EntityState& s) {
  const bool weather = entityId.rfind("weather.", 0) == 0;
  if (!attrs["friendly_name"].isNull()) s.name = attrs["friendly_name"] | "";
  if (weather) {
    if (!attrs["temperature_unit"].isNull()) s.unit = attrs["temperature_unit"] | "";
    if (!attrs["temperature"].isNull()) {
      std::string id;
      EntityState tmp;
      JsonDocument doc;
      doc["entity_id"] = entityId;
      doc["state"] = s.state;
      doc["attributes"]["temperature"] = attrs["temperature"];
      std::string json;
      serializeJson(doc, json);
      if (parseStateObject(json, id, tmp)) s.temperature = tmp.temperature;
    }
  } else if (!attrs["unit_of_measurement"].isNull()) {
    s.unit = attrs["unit_of_measurement"] | "";
  }
  if (entityId.rfind("light.", 0) == 0 && attrs["brightness"].is<int>()) {
    s.brightnessPct = static_cast<int8_t>((attrs["brightness"].as<int>() * 100 + 127) / 255);
  } else {
    for (JsonPairConst kv : attrs) {
      if (kv.key() == "brightness") s.brightnessPct = -1;  // present but not a level (null)
    }
  }
}

}  // namespace

bool splitUrl(const std::string& url, std::string& host, uint16_t& port) {
  if (url.rfind("http://", 0) != 0) return false;
  std::string rest = url.substr(7);
  const size_t slash = rest.find('/');
  if (slash != std::string::npos) rest.resize(slash);
  const size_t colon = rest.find(':');
  host = rest.substr(0, colon);
  if (host.empty()) return false;
  port = 80;
  if (colon != std::string::npos) {
    const std::string p = rest.substr(colon + 1);
    char* end = nullptr;
    const long v = std::strtol(p.c_str(), &end, 10);
    if (p.empty() || *end != '\0' || v <= 0 || v > 65535) return false;
    port = static_cast<uint16_t>(v);
  }
  return true;
}

bool applyEntitiesEvent(std::string_view eventJson, StateMap& m, std::vector<std::string>* changed) {
  JsonDocument doc;
  if (deserializeJson(doc, eventJson.data(), eventJson.size()) != DeserializationError::Ok || !doc.is<JsonObject>()) {
    return false;
  }
  const auto note = [&](const std::string& id, const EntityState& before) {
    if (changed && !sameShown(before, m[id])) changed->push_back(id);
  };
  for (JsonPairConst p : doc["a"].as<JsonObjectConst>()) {
    const std::string id = p.key().c_str();
    const EntityState before = m.count(id) ? m[id] : EntityState{};
    EntityState s;
    s.state = p.value()["s"] | "";
    s.forecast = before.forecast;
    applyAttributes(id, p.value()["a"], s);
    if (s.state != "on") s.brightnessPct = -1;
    m[id] = s;
    if (changed && (before.state.empty() || !sameShown(before, s))) changed->push_back(id);
  }
  for (JsonPairConst p : doc["c"].as<JsonObjectConst>()) {
    const std::string id = p.key().c_str();
    const EntityState before = m[id];
    EntityState& s = m[id];
    JsonObjectConst plus = p.value()["+"];
    if (!plus["s"].isNull()) s.state = plus["s"] | "";
    applyAttributes(id, plus["a"], s);
    for (JsonVariantConst removed : p.value()["-"]["a"].as<JsonArrayConst>()) {
      if (std::string_view(removed.as<const char*>() ? removed.as<const char*>() : "") == "brightness") {
        s.brightnessPct = -1;
      }
    }
    if (s.state != "on") s.brightnessPct = -1;
    note(id, before);
  }
  for (JsonVariantConst r : doc["r"].as<JsonArrayConst>()) {
    const std::string id = r.as<const char*>() ? r.as<const char*>() : "";
    if (id.empty()) continue;
    const EntityState before = m[id];
    m[id].state = "unavailable";
    note(id, before);
  }
  return true;
}

}  // namespace ha
