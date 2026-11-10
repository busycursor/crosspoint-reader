#include "HaConfigStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include "WifiCredentialStore.h"

namespace ha {
namespace {

// A long-lived access token is about 180 characters; leave room.
constexpr size_t kMaxTokenChars = 1024;
constexpr size_t kMaxConfigBytes = 16 * 1024;

bool isEnabled = false;
Config cfg;
std::string token;

// Rewrites the file with the token obfuscated. On failure the plain token
// stays in the file and is tried again next boot.
void storeTokenObfuscated() {
  cfg.tokenObf = obfuscation::obfuscateToBase64(cfg.token).c_str();
  const std::string tmp = std::string(kConfigPath) + ".tmp";
  if (!Storage.writeFile(tmp.c_str(), String(serializeConfig(cfg).c_str())) ||
      !Storage.replaceFile(tmp.c_str(), kConfigPath)) {
    LOG_ERR("HA", "HA config: could not rewrite %s", kConfigPath);
    return;
  }
  LOG_INF("HA", "HA config: token stored obfuscated");
}

}  // namespace

void loadConfigStore() {
  isEnabled = false;
#if defined(BOARD_HAS_PSRAM) || defined(SIMULATOR)
  if (!Storage.exists(kConfigPath)) return;
  std::string json;
  if (!Storage.readFileToString("HA", kConfigPath, kMaxConfigBytes, json)) {
    LOG_ERR("HA", "HA config: could not read %s", kConfigPath);
    return;
  }
  std::string error;
  if (!parseConfig(json, cfg, &error)) {
    LOG_ERR("HA", "HA config: %s", error.c_str());
    return;
  }
  if (!cfg.token.empty()) {
    token = cfg.token;
    storeTokenObfuscated();
    cfg.token.clear();
  } else {
    bool ok = false, tooLong = false;
    token = obfuscation::deobfuscateFromBase64(cfg.tokenObf.c_str(), kMaxTokenChars, &ok, &tooLong);
    if (!ok || tooLong || token.empty()) {
      LOG_ERR("HA", "HA config: stored token does not decode on this device; add \"token\" again");
      token.clear();
      return;
    }
  }
  WIFI_STORE.loadFromFile();
  if (!WIFI_STORE.findCredential(cfg.wifiSsid)) {
    LOG_ERR("HA", "HA config: wifi network \"%s\" is not saved", cfg.wifiSsid.c_str());
    return;
  }
  isEnabled = true;
  LOG_INF("HA", "HA config: enabled, %u sheet entries%s", static_cast<unsigned>(cfg.sheet.size()),
          cfg.dashboard.empty() ? "" : ", dashboard set");
#endif
}

bool enabled() { return isEnabled; }
const Config& config() { return cfg; }
const std::string& bearerToken() { return token; }

}  // namespace ha
