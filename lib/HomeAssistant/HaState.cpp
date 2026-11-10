#include "HaState.h"

#include <ArduinoJson.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ha {
namespace {

std::string_view domainOf(std::string_view entityId) {
  const size_t dot = entityId.find('.');
  return dot == std::string_view::npos ? std::string_view{} : entityId.substr(0, dot);
}

// A plain decimal number ("80.06", "-3", "12.0"); not "0:42:00" or "1e3".
bool parseNumber(const std::string& s, double& out) {
  if (s.empty()) return false;
  size_t i = s[0] == '-' ? 1 : 0;
  bool digits = false, dot = false;
  for (; i < s.size(); ++i) {
    if (s[i] >= '0' && s[i] <= '9') {
      digits = true;
    } else if (s[i] == '.' && !dot) {
      dot = true;
    } else {
      return false;
    }
  }
  if (!digits) return false;
  out = std::strtod(s.c_str(), nullptr);
  return true;
}

// At most one decimal, trailing ".0" dropped.
std::string formatNumber(double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f", v);
  std::string out = buf;
  if (out.size() > 2 && out.compare(out.size() - 2, 2, ".0") == 0) out.resize(out.size() - 2);
  if (out == "-0") out = "0";
  return out;
}

std::string withUnit(std::string value, const std::string& unit) {
  if (!unit.empty()) value += " " + unit;
  return value;
}

const char* conditionText(std::string_view c) {
  static constexpr struct {
    std::string_view key;
    const char* text;
  } kConditions[] = {
      {"clear-night", "Clear"},
      {"cloudy", "Cloudy"},
      {"exceptional", "Exceptional"},
      {"fog", "Fog"},
      {"hail", "Hail"},
      {"lightning", "Lightning"},
      {"lightning-rainy", "Storms"},
      {"partlycloudy", "Partly cloudy"},
      {"pouring", "Pouring"},
      {"rainy", "Rain"},
      {"snowy", "Snow"},
      {"snowy-rainy", "Sleet"},
      {"sunny", "Sunny"},
      {"windy", "Windy"},
      {"windy-variant", "Windy"},
  };
  for (const auto& k : kConditions) {
    if (k.key == c) return k.text;
  }
  return nullptr;
}

// Weekday of a "YYYY-MM-DD..." date (days-from-civil), "" when malformed.
const char* weekday(const char* datetime) {
  int y = 0, m = 0, d = 0;
  if (!datetime || std::sscanf(datetime, "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1) return "";
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = y - era * 400;
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long days = static_cast<long>(era) * 146097 + doe - 719468;  // 1970-01-01 was a Thursday
  static constexpr const char* kNames[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
  return kNames[((days % 7) + 7) % 7];
}

bool unavailable(const std::string& s) { return s.empty() || s == "unknown" || s == "unavailable"; }

}  // namespace

bool parseStateObject(std::string_view json, std::string& entityId, EntityState& out) {
  JsonDocument doc;
  if (deserializeJson(doc, json.data(), json.size()) != DeserializationError::Ok) return false;
  entityId = doc["entity_id"] | "";
  if (entityId.empty()) return false;
  JsonObjectConst a = doc["attributes"];
  EntityState s;
  s.state = doc["state"] | "";
  s.name = a["friendly_name"] | "";
  const std::string_view domain = domainOf(entityId);
  if (domain == "weather") {
    s.unit = a["temperature_unit"] | "";
    if (!a["temperature"].isNull()) s.temperature = formatNumber(a["temperature"].as<double>());
  } else {
    s.unit = a["unit_of_measurement"] | "";
  }
  if (domain == "light" && s.state == "on" && a["brightness"].is<int>()) {
    s.brightnessPct = static_cast<int8_t>((a["brightness"].as<int>() * 100 + 127) / 255);
  }
  out = std::move(s);
  return true;
}

std::string displayValue(const std::string& entityId, const EntityState& s) {
  // Actions: HA's state is only when they last ran.
  const std::string_view domain = domainOf(entityId);
  if (domain == "scene" || domain == "script" || domain == "button") return "";
  if (unavailable(s.state)) return "unavailable";
  if (domainOf(entityId) == "weather") {
    const char* text = conditionText(s.state);
    std::string out = text ? text : s.state;
    double t;
    if (parseNumber(s.temperature, t)) out += ", " + withUnit(std::to_string(std::lround(t)), s.unit);
    return out;
  }
  if (s.state == "on") return "On";
  if (s.state == "off") return "Off";
  double v;
  if (parseNumber(s.state, v)) return withUnit(formatNumber(v), s.unit);
  return withUnit(s.state, s.unit);
}

std::string forecastLine(std::string_view forecastArrayJson, int days) {
  JsonDocument doc;
  if (deserializeJson(doc, forecastArrayJson.data(), forecastArrayJson.size()) != DeserializationError::Ok) return "";
  std::string out;
  int n = 0;
  for (JsonObjectConst f : doc.as<JsonArrayConst>()) {
    if (n++ >= days) break;
    if (!out.empty()) out += "  ";
    out += weekday(f["datetime"]);
    out += " " + std::to_string(std::lround(f["temperature"] | 0.0));
    if (!f["templow"].isNull()) out += "/" + std::to_string(std::lround(f["templow"].as<double>()));
  }
  return out;
}

bool tapAction(const std::string& entityId, ServiceCall& out) {
  const std::string_view domain = domainOf(entityId);
  const char* service = nullptr;
  if (isToggleable(entityId)) {
    service = "toggle";
  } else if (domain == "scene" || domain == "script") {
    service = "turn_on";
  } else if (domain == "button") {
    service = "press";
  }
  if (!service) return false;
  out = ServiceCall{std::string(domain), service, entityId, -1};
  return true;
}

ServiceCall brightnessCall(const std::string& entityId, int pct) {
  return ServiceCall{"light", "turn_on", entityId, pct < 0 ? 0 : (pct > 100 ? 100 : pct)};
}

bool isToggleable(const std::string& entityId) {
  static constexpr std::string_view kToggle[] = {"light", "switch", "fan", "input_boolean", "media_player", "remote"};
  const std::string_view domain = domainOf(entityId);
  for (const auto d : kToggle) {
    if (domain == d) return true;
  }
  return false;
}

std::string serviceBody(const ServiceCall& call) {
  JsonDocument doc;
  doc["entity_id"] = call.entityId;
  if (call.brightnessPct >= 0) doc["brightness_pct"] = call.brightnessPct;
  std::string out;
  serializeJson(doc, out);
  return out;
}

std::string serializeStates(const StateMap& m) {
  JsonDocument doc;
  for (const auto& [id, s] : m) {
    JsonObject o = doc[id].to<JsonObject>();
    o["s"] = s.state;
    if (!s.name.empty()) o["n"] = s.name;
    if (!s.unit.empty()) o["u"] = s.unit;
    if (s.brightnessPct >= 0) o["b"] = s.brightnessPct;
    if (!s.forecast.empty()) o["f"] = s.forecast;
    if (!s.temperature.empty()) o["t"] = s.temperature;
  }
  std::string out;
  serializeJson(doc, out);
  return out;
}

bool parseStates(std::string_view json, StateMap& out) {
  JsonDocument doc;
  if (deserializeJson(doc, json.data(), json.size()) != DeserializationError::Ok || !doc.is<JsonObject>()) return false;
  StateMap m;
  for (JsonPairConst p : doc.as<JsonObjectConst>()) {
    JsonObjectConst o = p.value();
    EntityState s;
    s.state = o["s"] | "";
    s.name = o["n"] | "";
    s.unit = o["u"] | "";
    s.brightnessPct = static_cast<int8_t>(o["b"] | -1);
    s.forecast = o["f"] | "";
    s.temperature = o["t"] | "";
    m[p.key().c_str()] = std::move(s);
  }
  out = std::move(m);
  return true;
}

}  // namespace ha
