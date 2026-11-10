#include <gtest/gtest.h>

#include "HaLive.h"

TEST(HaUrl, SplitsHostAndPort) {
  std::string host;
  uint16_t port = 0;
  ASSERT_TRUE(ha::splitUrl("http://homeassistant.local:8123", host, port));
  EXPECT_EQ(host, "homeassistant.local");
  EXPECT_EQ(port, 8123);
  ASSERT_TRUE(ha::splitUrl("http://10.0.0.5", host, port));
  EXPECT_EQ(host, "10.0.0.5");
  EXPECT_EQ(port, 80);
  EXPECT_FALSE(ha::splitUrl("http://:8123", host, port));
  EXPECT_FALSE(ha::splitUrl("http://ha:notaport", host, port));
  EXPECT_FALSE(ha::splitUrl("https://ha", host, port));
}

TEST(HaLive, AdditionsAreFullStates) {
  ha::StateMap m;
  std::vector<std::string> changed;
  ASSERT_TRUE(ha::applyEntitiesEvent(
      R"({"a":{"light.a":{"s":"on","a":{"friendly_name":"Lamp","brightness":255},"c":"x","lc":1.0},
               "sensor.t":{"s":"68.4","a":{"unit_of_measurement":"°F","friendly_name":"Out"}}}})",
      m, &changed));
  EXPECT_EQ(changed.size(), 2u);
  EXPECT_EQ(m["light.a"].state, "on");
  EXPECT_EQ(m["light.a"].brightnessPct, 100);
  EXPECT_EQ(m["light.a"].name, "Lamp");
  EXPECT_EQ(m["sensor.t"].unit, "\xC2\xB0" "F");
}

TEST(HaLive, ChangesPatchState) {
  ha::StateMap m;
  ha::applyEntitiesEvent(R"({"a":{"light.a":{"s":"on","a":{"friendly_name":"Lamp","brightness":255}}}})", m,
                         nullptr);
  std::vector<std::string> changed;
  ASSERT_TRUE(ha::applyEntitiesEvent(R"({"c":{"light.a":{"+":{"a":{"brightness":128},"lc":2.0}}}})", m, &changed));
  ASSERT_EQ(changed.size(), 1u);
  EXPECT_EQ(m["light.a"].brightnessPct, 50);
  EXPECT_EQ(m["light.a"].name, "Lamp");

  ha::applyEntitiesEvent(R"({"c":{"light.a":{"+":{"s":"off"},"-":{"a":["brightness"]}}}})", m, nullptr);
  EXPECT_EQ(m["light.a"].state, "off");
  EXPECT_EQ(m["light.a"].brightnessPct, -1);
}

TEST(HaLive, ContextOnlyChangeIsNotAChange) {
  ha::StateMap m;
  ha::applyEntitiesEvent(R"({"a":{"sensor.t":{"s":"1","a":{}}}})", m, nullptr);
  std::vector<std::string> changed;
  ha::applyEntitiesEvent(R"({"c":{"sensor.t":{"+":{"lu":3.0,"c":"y"}}}})", m, &changed);
  EXPECT_TRUE(changed.empty());
}

TEST(HaLive, RemovalsMarkUnavailable) {
  ha::StateMap m;
  ha::applyEntitiesEvent(R"({"a":{"sensor.t":{"s":"1","a":{}}}})", m, nullptr);
  std::vector<std::string> changed;
  ha::applyEntitiesEvent(R"({"r":["sensor.t"]})", m, &changed);
  ASSERT_EQ(changed.size(), 1u);
  EXPECT_EQ(m["sensor.t"].state, "unavailable");
}

TEST(HaLive, WeatherTemperatureFromAttributes) {
  ha::StateMap m;
  ha::applyEntitiesEvent(
      R"({"a":{"weather.home":{"s":"sunny","a":{"temperature":71.6,"temperature_unit":"°F"}}}})", m, nullptr);
  EXPECT_EQ(ha::displayValue("weather.home", m["weather.home"]), "Sunny, 72 \xC2\xB0" "F");
}

TEST(HaLive, InvalidJsonFails) {
  ha::StateMap m;
  EXPECT_FALSE(ha::applyEntitiesEvent("nope", m, nullptr));
}
