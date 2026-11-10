#include <gtest/gtest.h>

#include "HaConfig.h"

namespace {

const char* kExample = R"({
  "url": "http://homeassistant.local:8123",
  "token": "abc.def.ghi",
  "wifi": "HomeNetwork",
  "sheet": [
    { "entity": "light.reading_lamp", "name": "Reading lamp" },
    { "entity": "light.desk_lamp" },
    { "entity": "scene.evening" }
  ],
  "dashboard": "dashboard-ereader"
})";

ha::Config parseOk(const std::string& json) {
  ha::Config cfg;
  std::string error;
  EXPECT_TRUE(ha::parseConfig(json, cfg, &error)) << error;
  return cfg;
}

std::string parseError(const std::string& json) {
  ha::Config cfg;
  std::string error;
  EXPECT_FALSE(ha::parseConfig(json, cfg, &error));
  return error;
}

}  // namespace

TEST(HaConfig, ParsesFullExample) {
  const auto cfg = parseOk(kExample);
  EXPECT_EQ(cfg.url, "http://homeassistant.local:8123");
  EXPECT_EQ(cfg.token, "abc.def.ghi");
  EXPECT_TRUE(cfg.tokenObf.empty());
  EXPECT_EQ(cfg.wifiSsid, "HomeNetwork");
  EXPECT_EQ(cfg.dashboard, "dashboard-ereader");
  ASSERT_EQ(cfg.sheet.size(), 3u);
  EXPECT_EQ(cfg.sheet[0].entity, "light.reading_lamp");
  EXPECT_EQ(cfg.sheet[0].name, "Reading lamp");
  EXPECT_EQ(cfg.sheet[0].kind, ha::SheetKind::Light);
  EXPECT_EQ(cfg.sheet[1].name, "");
  EXPECT_EQ(cfg.sheet[1].kind, ha::SheetKind::Light);
  EXPECT_EQ(cfg.sheet[2].kind, ha::SheetKind::Button);
}

TEST(HaConfig, StripsTrailingSlash) {
  const auto cfg = parseOk(R"({"url":"http://ha.lan:8123/","token":"t","wifi":"W"})");
  EXPECT_EQ(cfg.url, "http://ha.lan:8123");
  EXPECT_TRUE(cfg.dashboard.empty());
  EXPECT_TRUE(cfg.sheet.empty());
}

TEST(HaConfig, RejectsHttpsUrl) {
  EXPECT_NE(parseError(R"({"url":"https://ha.lan","token":"t","wifi":"W"})").find("http://"), std::string::npos);
}

TEST(HaConfig, RequiresWifi) {
  EXPECT_NE(parseError(R"({"url":"http://ha.lan","token":"t"})").find("wifi"), std::string::npos);
}

TEST(HaConfig, RequiresTokenOrTokenObf) {
  EXPECT_NE(parseError(R"({"url":"http://ha.lan","wifi":"W"})").find("token"), std::string::npos);
  const auto cfg = parseOk(R"({"url":"http://ha.lan","wifi":"W","token_obf":"QUJD"})");
  EXPECT_EQ(cfg.tokenObf, "QUJD");
  EXPECT_TRUE(cfg.token.empty());
}

TEST(HaConfig, RejectsInvalidJson) { EXPECT_FALSE(parseError("{\"url\": ").empty()); }

TEST(HaConfig, AcceptsBomAndCrlf) {
  const auto cfg = parseOk("\xEF\xBB\xBF{\r\n\"url\": \"http://ha.lan\",\r\n\"token\": \"t\",\r\n\"wifi\": \"W\"\r\n}\r\n");
  EXPECT_EQ(cfg.url, "http://ha.lan");
}

TEST(HaConfig, IgnoresUnknownKeys) {
  const auto cfg = parseOk(R"({"url":"http://ha.lan","token":"t","wifi":"W","theme":"dark","sheet":[{"entity":"light.a","color":"red"}]})");
  ASSERT_EQ(cfg.sheet.size(), 1u);
}

TEST(HaConfig, CapsSheet) {
  std::string json = R"({"url":"http://ha.lan","token":"t","wifi":"W","sheet":[)";
  for (int i = 0; i < 6; ++i) json += "{\"entity\":\"light.l" + std::to_string(i) + "\"},";
  for (int i = 0; i < 7; ++i) json += "{\"entity\":\"scene.s" + std::to_string(i) + "\"},";
  json.pop_back();
  json += "]}";
  const auto cfg = parseOk(json);
  ASSERT_EQ(cfg.sheet.size(), ha::kMaxSheetLights + ha::kMaxSheetButtons);
  EXPECT_EQ(cfg.sheet[0].entity, "light.l0");
  EXPECT_EQ(cfg.sheet[3].entity, "light.l3");
  EXPECT_EQ(cfg.sheet[4].entity, "scene.s0");
  EXPECT_EQ(cfg.sheet[8].entity, "scene.s4");
}

TEST(HaConfig, SkipsUnsupportedDomain) {
  const auto cfg = parseOk(
      R"({"url":"http://ha.lan","token":"t","wifi":"W","sheet":[{"entity":"sensor.x"},{"entity":"switch.fan"},{"entity":"bogus"}]})");
  ASSERT_EQ(cfg.sheet.size(), 1u);
  EXPECT_EQ(cfg.sheet[0].entity, "switch.fan");
  EXPECT_EQ(cfg.sheet[0].kind, ha::SheetKind::Button);
}

TEST(HaConfig, SerializeDropsPlainToken) {
  auto cfg = parseOk(kExample);
  cfg.tokenObf = "T0JG";
  const std::string out = ha::serializeConfig(cfg);
  EXPECT_EQ(out.find("\"token\""), std::string::npos);
  EXPECT_EQ(out.find("abc.def.ghi"), std::string::npos);
  const auto back = parseOk(out);
  EXPECT_EQ(back.tokenObf, "T0JG");
  EXPECT_TRUE(back.token.empty());
  EXPECT_EQ(back.url, cfg.url);
  EXPECT_EQ(back.wifiSsid, cfg.wifiSsid);
  EXPECT_EQ(back.dashboard, cfg.dashboard);
  ASSERT_EQ(back.sheet.size(), 3u);
  EXPECT_EQ(back.sheet[0].name, "Reading lamp");
  EXPECT_EQ(back.sheet[2].entity, "scene.evening");
}
