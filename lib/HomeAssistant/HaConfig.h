#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Home Assistant settings from /homeassistant.json (see
// docs/superpowers/specs/2026-11-10-home-assistant-design.md). Pure parsing:
// token obfuscation and file I/O live in src/homeassistant/HaConfigStore.
namespace ha {

enum class SheetKind : uint8_t { Light, Button };

// One control in the reader's swipe-down sheet.
struct SheetEntry {
  std::string entity;
  std::string name;  // empty: use HA's friendly name
  SheetKind kind = SheetKind::Button;
};

struct Config {
  std::string url;        // http://host[:port], no trailing slash
  std::string token;      // plain; only when the file had "token"
  std::string tokenObf;   // "token_obf" as stored
  std::string wifiSsid;   // the home network
  std::string dashboard;  // dashboard url_path; empty: no Control page
  std::vector<SheetEntry> sheet;
};

// Room the sheet has; extra entries are skipped.
constexpr size_t kMaxSheetLights = 4;
constexpr size_t kMaxSheetButtons = 5;

bool parseConfig(std::string_view json, Config& out, std::string* error);
// The same settings, without "token"; "token_obf" comes from cfg.tokenObf.
std::string serializeConfig(const Config& cfg);

}  // namespace ha
