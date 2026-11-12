#include <gtest/gtest.h>

#include "VikunjaConfig.h"

namespace {

const char* kExample = R"({
  "url": "https://todo.example.com/",
  "token": "tk_abc",
  "headers": [
    { "name": "CF-Access-Client-Id", "value": "id.access" },
    { "name": "CF-Access-Client-Secret", "value": "secret" }
  ]
})";

vikunja::Config parseOk(const std::string& json) {
  vikunja::Config cfg;
  std::string error;
  EXPECT_TRUE(vikunja::parseConfig(json, cfg, &error)) << error;
  return cfg;
}

std::string parseError(const std::string& json) {
  vikunja::Config cfg;
  std::string error;
  EXPECT_FALSE(vikunja::parseConfig(json, cfg, &error));
  return error;
}

}  // namespace

TEST(VikunjaConfig, ParsesUrlTokenHeaders) {
  const auto cfg = parseOk(kExample);
  EXPECT_EQ(cfg.url, "https://todo.example.com");
  EXPECT_EQ(cfg.token, "tk_abc");
  EXPECT_TRUE(cfg.tokenObf.empty());
  ASSERT_EQ(cfg.headers.size(), 2u);
  EXPECT_EQ(cfg.headers[0].name, "CF-Access-Client-Id");
  EXPECT_EQ(cfg.headers[0].value, "id.access");
  EXPECT_EQ(cfg.headers[1].name, "CF-Access-Client-Secret");
  EXPECT_EQ(cfg.headers[1].value, "secret");
}

TEST(VikunjaConfig, AcceptsPlainHttp) {
  EXPECT_EQ(parseOk(R"({"url":"http://nas:3456","token":"t"})").url, "http://nas:3456");
}

TEST(VikunjaConfig, RejectsMissingUrl) { EXPECT_FALSE(parseError(R"({"token":"t"})").empty()); }

TEST(VikunjaConfig, RejectsNonHttpScheme) { EXPECT_FALSE(parseError(R"({"url":"ftp://x","token":"t"})").empty()); }

TEST(VikunjaConfig, RejectsUrlWithoutHost) { EXPECT_FALSE(parseError(R"({"url":"https://","token":"t"})").empty()); }

TEST(VikunjaConfig, RejectsNoToken) { EXPECT_FALSE(parseError(R"({"url":"https://x"})").empty()); }

TEST(VikunjaConfig, RejectsNonObject) { EXPECT_FALSE(parseError("[1,2]").empty()); }

TEST(VikunjaConfig, KeepsObfFieldsWhenNoPlain) {
  const auto cfg = parseOk(R"({"url":"https://x","token_obf":"T0JG",
    "headers":[{"name":"A","value_obf":"VkFM"}]})");
  EXPECT_TRUE(cfg.token.empty());
  EXPECT_EQ(cfg.tokenObf, "T0JG");
  ASSERT_EQ(cfg.headers.size(), 1u);
  EXPECT_TRUE(cfg.headers[0].value.empty());
  EXPECT_EQ(cfg.headers[0].valueObf, "VkFM");
}

TEST(VikunjaConfig, ExtraHeadersIgnored) {
  const auto cfg = parseOk(R"({"url":"https://x","token":"t",
    "headers":[{"name":"A","value":"1"},{"name":"B","value":"2"},{"name":"C","value":"3"}]})");
  EXPECT_EQ(cfg.headers.size(), vikunja::kMaxHeaders);
}

TEST(VikunjaConfig, NamelessHeaderSkipped) {
  const auto cfg = parseOk(R"({"url":"https://x","token":"t","headers":[{"value":"1"},{"name":"B","value":"2"}]})");
  ASSERT_EQ(cfg.headers.size(), 1u);
  EXPECT_EQ(cfg.headers[0].name, "B");
}

TEST(VikunjaConfig, SerializeOmitsPlainValues) {
  auto cfg = parseOk(kExample);
  cfg.tokenObf = "TOKOBF";
  cfg.headers[0].valueObf = "V0";
  cfg.headers[1].valueObf = "V1";
  const std::string out = vikunja::serializeConfig(cfg);
  EXPECT_NE(out.find("\"token_obf\""), std::string::npos);
  EXPECT_NE(out.find("\"value_obf\""), std::string::npos);
  EXPECT_EQ(out.find("\"token\":"), std::string::npos);
  EXPECT_EQ(out.find("\"value\":"), std::string::npos);
  EXPECT_EQ(out.find("tk_abc"), std::string::npos);
  EXPECT_EQ(out.find("secret"), std::string::npos);

  const auto back = parseOk(out);
  EXPECT_EQ(back.url, "https://todo.example.com");
  EXPECT_EQ(back.tokenObf, "TOKOBF");
  ASSERT_EQ(back.headers.size(), 2u);
  EXPECT_EQ(back.headers[1].name, "CF-Access-Client-Secret");
  EXPECT_EQ(back.headers[1].valueObf, "V1");
}
