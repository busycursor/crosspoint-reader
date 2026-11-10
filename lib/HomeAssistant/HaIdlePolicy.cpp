#include "HaIdlePolicy.h"

namespace ha {

LinkStatus statusFromHttp(const int httpCode) {
  if (httpCode == 401 || httpCode == 403) return LinkStatus::TokenRejected;
  if (httpCode >= 200 && httpCode < 500) return LinkStatus::Ok;
  return LinkStatus::NoAnswer;
}

void IdlePolicy::noteJoined(const bool wifiWasAlreadyUp, const uint32_t nowMs) {
  if (!wifiWasAlreadyUp) owns_ = true;
  lastActivityMs_ = nowMs;
}

void IdlePolicy::noteActivity(const uint32_t nowMs) { lastActivityMs_ = nowMs; }

void IdlePolicy::noteSleep() { owns_ = false; }

bool IdlePolicy::shouldRelease(const uint32_t nowMs, const bool developerMode, const bool activityAllows) {
  if (!owns_ || developerMode || !activityAllows) return false;
  if (nowMs - lastActivityMs_ < kWifiIdleOffMs) return false;
  owns_ = false;
  return true;
}

}  // namespace ha
