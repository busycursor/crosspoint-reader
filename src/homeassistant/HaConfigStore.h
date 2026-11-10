#pragma once

#include <HaConfig.h>

#include <string>

// Home Assistant settings loaded from /homeassistant.json at boot. A plain
// "token" in the file is stored obfuscated (keyed to this chip) and the file
// is rewritten without it.
namespace ha {

constexpr const char* kConfigPath = "/homeassistant.json";

void loadConfigStore();
// PSRAM board, a valid file, a token that decodes, and a saved home network.
bool enabled();
// Only meaningful when enabled().
const Config& config();
const std::string& bearerToken();

}  // namespace ha
