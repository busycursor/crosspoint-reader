#include "HaConfig.h"

#include <ArduinoJson.h>

namespace ha {
namespace {

bool fail(std::string* error, const char* message) {
  if (error) *error = message;
  return false;
}

// Sheet kind by domain; false for domains the sheet cannot show.
bool sheetKind(std::string_view entity, SheetKind& kind) {
  const size_t dot = entity.find('.');
  if (dot == std::string_view::npos || dot == 0 || dot + 1 == entity.size()) return false;
  const std::string_view domain = entity.substr(0, dot);
  if (domain == "light") {
    kind = SheetKind::Light;
    return true;
  }
  static constexpr std::string_view kButtonDomains[] = {"scene", "script",       "switch", "button",
                                                        "fan",   "input_boolean", "remote", "media_player"};
  for (const auto d : kButtonDomains) {
    if (domain == d) {
      kind = SheetKind::Button;
      return true;
    }
  }
  return false;
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
  cfg.wifiSsid = doc["wifi"] | "";
  cfg.dashboard = doc["dashboard"] | "";
  if (cfg.url.rfind("http://", 0) != 0) return fail(error, "url must start with http://");
  while (cfg.url.size() > 7 && cfg.url.back() == '/') cfg.url.pop_back();
  if (cfg.url.size() <= 7) return fail(error, "url has no host");
  if (cfg.token.empty() && cfg.tokenObf.empty()) return fail(error, "no token");
  if (cfg.wifiSsid.empty()) return fail(error, "no wifi network");

  size_t lights = 0, buttons = 0;
  for (JsonObjectConst item : doc["sheet"].as<JsonArrayConst>()) {
    SheetEntry entry;
    entry.entity = item["entity"] | "";
    entry.name = item["name"] | "";
    if (!sheetKind(entry.entity, entry.kind)) continue;
    size_t& count = entry.kind == SheetKind::Light ? lights : buttons;
    if (count >= (entry.kind == SheetKind::Light ? kMaxSheetLights : kMaxSheetButtons)) continue;
    ++count;
    cfg.sheet.push_back(std::move(entry));
  }
  out = std::move(cfg);
  return true;
}

std::string serializeConfig(const Config& cfg) {
  JsonDocument doc;
  doc["url"] = cfg.url;
  doc["token_obf"] = cfg.tokenObf;
  doc["wifi"] = cfg.wifiSsid;
  JsonArray sheet = doc["sheet"].to<JsonArray>();
  for (const auto& entry : cfg.sheet) {
    JsonObject item = sheet.add<JsonObject>();
    item["entity"] = entry.entity;
    if (!entry.name.empty()) item["name"] = entry.name;
  }
  if (!cfg.dashboard.empty()) doc["dashboard"] = cfg.dashboard;
  std::string out;
  serializeJsonPretty(doc, out);
  out += '\n';
  return out;
}

}  // namespace ha
