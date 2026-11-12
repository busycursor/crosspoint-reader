#include "VikunjaConfig.h"

#include <ArduinoJson.h>

namespace vikunja {
namespace {

bool fail(std::string* error, const char* message) {
  if (error) *error = message;
  return false;
}

// Length of the scheme prefix, or 0 when the URL is not http(s).
size_t schemeLength(const std::string& url) {
  if (url.rfind("https://", 0) == 0) return 8;
  if (url.rfind("http://", 0) == 0) return 7;
  return 0;
}

}  // namespace

bool parseConfig(std::string_view json, Config& out, std::string* error) {
  if (json.size() >= 3 && json.substr(0, 3) == "\xEF\xBB\xBF") json.remove_prefix(3);
  JsonDocument doc;
  if (deserializeJson(doc, json.data(), json.size()) != DeserializationError::Ok || !doc.is<JsonObject>()) {
    return fail(error, "not a JSON object");
  }
  Config cfg;
  cfg.url = doc["url"] | "";
  cfg.token = doc["token"] | "";
  cfg.tokenObf = doc["token_obf"] | "";
  const size_t scheme = schemeLength(cfg.url);
  if (scheme == 0) return fail(error, "url must start with https:// or http://");
  while (cfg.url.size() > scheme && cfg.url.back() == '/') cfg.url.pop_back();
  if (cfg.url.size() <= scheme) return fail(error, "url has no host");
  if (cfg.token.empty() && cfg.tokenObf.empty()) return fail(error, "no token");

  for (JsonObjectConst item : doc["headers"].as<JsonArrayConst>()) {
    if (cfg.headers.size() >= kMaxHeaders) break;
    Header header;
    header.name = item["name"] | "";
    header.value = item["value"] | "";
    header.valueObf = item["value_obf"] | "";
    if (header.name.empty()) continue;
    cfg.headers.push_back(std::move(header));
  }
  out = std::move(cfg);
  return true;
}

std::string serializeConfig(const Config& cfg) {
  JsonDocument doc;
  doc["url"] = cfg.url;
  doc["token_obf"] = cfg.tokenObf;
  if (!cfg.headers.empty()) {
    JsonArray headers = doc["headers"].to<JsonArray>();
    for (const auto& header : cfg.headers) {
      JsonObject item = headers.add<JsonObject>();
      item["name"] = header.name;
      item["value_obf"] = header.valueObf;
    }
  }
  std::string out;
  serializeJsonPretty(doc, out);
  return out;
}

}  // namespace vikunja
