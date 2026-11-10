#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

// Home Assistant entity states as the device shows them, the service calls a
// tap or slider sends, and the cached-state file format.
namespace ha {

struct EntityState {
  std::string state;          // raw HA state ("on", "80.06", "partlycloudy", ...)
  std::string name;           // friendly_name
  std::string unit;           // unit_of_measurement (weather: temperature_unit)
  int8_t brightnessPct = -1;  // lights: 0..100; -1 when off or not a light
  std::string forecast;       // weather: preformatted forecastLine()
  std::string temperature;    // weather: current temperature as HA sent it
};

using StateMap = std::map<std::string, EntityState>;

// One HA state object: {"entity_id", "state", "attributes"}.
bool parseStateObject(std::string_view json, std::string& entityId, EntityState& out);
// What a card shows as the value: "On", "80.1 °F", "Partly cloudy, 72 °F",
// "unavailable" for unknown or missing states, nothing for scenes, scripts
// and buttons.
std::string displayValue(const std::string& entityId, const EntityState& s);
// Daily forecast entries (weather.get_forecasts) as "Wed 70/55  Thu 68/52".
std::string forecastLine(std::string_view forecastArrayJson, int days);

struct ServiceCall {
  std::string domain;
  std::string service;
  std::string entityId;
  int brightnessPct = -1;  // light.turn_on only
};

// The tap action for an entity; false for read-only domains.
bool tapAction(const std::string& entityId, ServiceCall& out);
ServiceCall brightnessCall(const std::string& entityId, int pct);
bool isToggleable(const std::string& entityId);
// JSON body for POST /api/services/<domain>/<service>.
std::string serviceBody(const ServiceCall& call);

std::string serializeStates(const StateMap& m);
bool parseStates(std::string_view json, StateMap& out);

}  // namespace ha
