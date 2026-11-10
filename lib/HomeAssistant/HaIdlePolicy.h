#pragma once

#include <cstdint>

// When the Home Assistant link may turn Wi-Fi off again, and how an HTTP
// result maps to what the sheet and Control page show. Pure: times are the
// caller's millis().
namespace ha {

enum class LinkStatus : uint8_t { Idle, Working, Ok, NoNetwork, NoAnswer, TokenRejected };

// 2xx and 4xx other than 401/403 mean HA answered (a 404 is a missing
// entity, not a broken link); <= 0 (timeout, refused) and 5xx mean it did not.
LinkStatus statusFromHttp(int httpCode);

constexpr uint32_t kWifiIdleOffMs = 120000;

class IdlePolicy {
 public:
  // After a successful join; wifiWasAlreadyUp: someone else owns Wi-Fi.
  void noteJoined(bool wifiWasAlreadyUp, uint32_t nowMs);
  void noteActivity(uint32_t nowMs);
  // Sleep turned Wi-Fi off: nothing to release any more.
  void noteSleep();
  // True once, after kWifiIdleOffMs without activity, when this link turned
  // Wi-Fi on, Developer Mode is off and the current screen allows it.
  bool shouldRelease(uint32_t nowMs, bool developerMode, bool activityAllows);
  bool ownsWifi() const { return owns_; }

 private:
  bool owns_ = false;
  uint32_t lastActivityMs_ = 0;
};

}  // namespace ha
