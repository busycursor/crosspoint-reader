#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Vikunja settings from /vikunja.json (see
// docs/superpowers/specs/2026-11-11-vikunja-lists-design.md). Pure parsing:
// obfuscation and file I/O live in src/vikunja/VikunjaStore.
namespace vikunja {

// An extra request header, e.g. a Cloudflare Access service token.
struct Header {
  std::string name;
  std::string value;     // plain; only when the file had "value"
  std::string valueObf;  // "value_obf" as stored
};

struct Config {
  std::string url;       // http(s)://host[:port][/path], no trailing slash
  std::string token;     // plain; only when the file had "token"
  std::string tokenObf;  // "token_obf" as stored
  std::vector<Header> headers;
};

// Same as OpdsServer::MAX_CUSTOM_HEADERS.
constexpr size_t kMaxHeaders = 2;

bool parseConfig(std::string_view json, Config& out, std::string* error);
// The same settings with only the obfuscated token and header values.
std::string serializeConfig(const Config& cfg);

}  // namespace vikunja
