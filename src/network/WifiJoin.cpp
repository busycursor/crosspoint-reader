#include "WifiJoin.h"

#include <Arduino.h>
#include <Logging.h>
#include <WiFi.h>

#include "WifiCredentialStore.h"

bool joinSavedWifi(const unsigned long timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) return true;
  WIFI_STORE.loadFromFile();
  const std::string last = WIFI_STORE.getLastConnectedSsid();
  const auto cred = last.empty() ? WIFI_STORE.getCredentialAt(0) : WIFI_STORE.findCredential(last);
  if (!cred) {
    LOG_INF("WIFI", "No saved network to join");
    return false;
  }
  LOG_INF("WIFI", "Joining saved network %s (status %d, mode %d)", cred->ssid.c_str(), static_cast<int>(WiFi.status()),
          static_cast<int>(WiFi.getMode()));
  WiFi.mode(WIFI_STA);
  WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) delay(100);
  const bool ok = WiFi.status() == WL_CONNECTED;
  LOG_INF("WIFI", "Join %s after %lu ms (status %d)", ok ? "succeeded" : "failed", millis() - start,
          static_cast<int>(WiFi.status()));
  return ok;
}
