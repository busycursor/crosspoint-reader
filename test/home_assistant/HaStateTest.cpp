#include <gtest/gtest.h>

#include "HaState.h"

namespace {

ha::EntityState parseState(const std::string& json, const char* expectedId) {
  std::string id;
  ha::EntityState s;
  EXPECT_TRUE(ha::parseStateObject(json, id, s));
  EXPECT_EQ(id, expectedId);
  return s;
}

std::string shown(const char* entity, const std::string& state, const std::string& unit = "") {
  ha::EntityState s;
  s.state = state;
  s.unit = unit;
  return ha::displayValue(entity, s);
}

}  // namespace

TEST(HaFormat, OnOff) {
  EXPECT_EQ(shown("light.a", "on"), "On");
  EXPECT_EQ(shown("switch.a", "off"), "Off");
}

TEST(HaFormat, NumberWithUnit) {
  EXPECT_EQ(shown("sensor.t", "80.06",
                  "\xC2\xB0"
                  "F"),
            "80.1 \xC2\xB0"
            "F");
  EXPECT_EQ(shown("sensor.h", "62", "%"), "62 %");
  EXPECT_EQ(shown("sensor.p", "12.0", "W"), "12 W");
  EXPECT_EQ(shown("sensor.n", "-3.26"), "-3.3");
}

TEST(HaFormat, TextStatesPassThrough) {
  EXPECT_EQ(shown("sensor.appliance_status", "power_off"), "power_off");
  EXPECT_EQ(shown("sensor.left", "0:42:00"), "0:42:00");
}

TEST(HaFormat, ActionsShowNoValue) {
  EXPECT_EQ(shown("scene.evening", "unknown"), "");
  EXPECT_EQ(shown("script.bedtime", "off"), "");
  EXPECT_EQ(shown("button.restart", "2026-11-10T08:00:00+00:00"), "");
}

TEST(HaFormat, MissingEntityIsUnavailable) {
  EXPECT_EQ(shown("sensor.a", "unknown", "W"), "unavailable");
  EXPECT_EQ(shown("sensor.a", "unavailable"), "unavailable");
  EXPECT_EQ(shown("sensor.a", ""), "unavailable");
}

TEST(HaFormat, LightBrightnessFromAttributes) {
  auto s = parseState(R"({"entity_id":"light.a","state":"on","attributes":{"friendly_name":"Lamp","brightness":128}})",
                      "light.a");
  EXPECT_EQ(s.name, "Lamp");
  EXPECT_EQ(s.brightnessPct, 50);
  s = parseState(R"({"entity_id":"light.a","state":"off","attributes":{"brightness":null}})", "light.a");
  EXPECT_EQ(s.brightnessPct, -1);
  s = parseState(R"({"entity_id":"light.full","state":"on","attributes":{"brightness":255}})", "light.full");
  EXPECT_EQ(s.brightnessPct, 100);
}

TEST(HaFormat, UnitFromAttributes) {
  const auto s = parseState(
      R"({"entity_id":"sensor.t","state":"80.06","attributes":{"unit_of_measurement":"°F","friendly_name":"Outdoor"}})",
      "sensor.t");
  EXPECT_EQ(ha::displayValue("sensor.t", s),
            "80.1 \xC2\xB0"
            "F");
}

TEST(HaFormat, WeatherConditionAndTemp) {
  const auto s = parseState(
      R"({"entity_id":"weather.home","state":"partlycloudy","attributes":{"temperature":71.6,"temperature_unit":"°F"}})",
      "weather.home");
  EXPECT_EQ(ha::displayValue("weather.home", s),
            "Partly cloudy, 72 \xC2\xB0"
            "F");
}

TEST(HaFormat, ForecastLine) {
  const std::string line = ha::forecastLine(R"([
    {"datetime":"2026-11-11T08:00:00+00:00","temperature":70.2,"templow":55.1,"condition":"sunny"},
    {"datetime":"2026-11-12T08:00:00+00:00","temperature":68,"templow":52,"condition":"rainy"}])",
                                            2);
  EXPECT_EQ(line, "Wed 70/55  Thu 68/52");
}

TEST(HaAction, TapActionsByDomain) {
  const struct {
    const char* entity;
    const char* domain;
    const char* service;
  } cases[] = {
      {"light.a", "light", "toggle"},
      {"switch.a", "switch", "toggle"},
      {"fan.a", "fan", "toggle"},
      {"input_boolean.a", "input_boolean", "toggle"},
      {"media_player.a", "media_player", "toggle"},
      {"remote.a", "remote", "toggle"},
      {"scene.a", "scene", "turn_on"},
      {"script.a", "script", "turn_on"},
      {"button.a", "button", "press"},
  };
  for (const auto& c : cases) {
    ha::ServiceCall call;
    ASSERT_TRUE(ha::tapAction(c.entity, call)) << c.entity;
    EXPECT_EQ(call.domain, c.domain);
    EXPECT_EQ(call.service, c.service);
    EXPECT_EQ(call.entityId, c.entity);
    EXPECT_EQ(call.brightnessPct, -1);
  }
}

TEST(HaAction, SensorHasNoTap) {
  ha::ServiceCall call;
  EXPECT_FALSE(ha::tapAction("sensor.a", call));
  EXPECT_FALSE(ha::tapAction("binary_sensor.a", call));
  EXPECT_FALSE(ha::tapAction("weather.a", call));
  EXPECT_FALSE(ha::isToggleable("scene.a"));
  EXPECT_TRUE(ha::isToggleable("light.a"));
}

TEST(HaAction, BrightnessCall) {
  const auto call = ha::brightnessCall("light.a", 35);
  EXPECT_EQ(call.domain, "light");
  EXPECT_EQ(call.service, "turn_on");
  EXPECT_EQ(call.brightnessPct, 35);
  EXPECT_EQ(ha::serviceBody(call), R"({"entity_id":"light.a","brightness_pct":35})");
  EXPECT_EQ(ha::serviceBody(ha::ServiceCall{"scene", "turn_on", "scene.b"}), R"({"entity_id":"scene.b"})");
}

TEST(HaStates, RoundTrip) {
  ha::StateMap m;
  m["light.a"] = ha::EntityState{"on", "Lamp", "", 40, "", ""};
  m["sensor.t"] = ha::EntityState{"80.06",
                                  "Outdoor",
                                  "\xC2\xB0"
                                  "F",
                                  -1,
                                  "",
                                  ""};
  m["weather.home"] = ha::EntityState{"sunny",
                                      "",
                                      "\xC2\xB0"
                                      "F",
                                      -1,
                                      "Wed 70/55",
                                      ""};
  ha::StateMap back;
  ASSERT_TRUE(ha::parseStates(ha::serializeStates(m), back));
  ASSERT_EQ(back.size(), 3u);
  EXPECT_EQ(back["light.a"].brightnessPct, 40);
  EXPECT_EQ(back["light.a"].name, "Lamp");
  EXPECT_EQ(back["sensor.t"].unit,
            "\xC2\xB0"
            "F");
  EXPECT_EQ(back["weather.home"].forecast, "Wed 70/55");
  EXPECT_FALSE(ha::parseStates("garbage", back));
}
