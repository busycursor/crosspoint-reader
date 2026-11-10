#include "HaLink.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Logging.h>
#include <NetworkClient.h>
#include <WiFi.h>

#include <mutex>

#include "CrossPointSettings.h"
#include "activities/ActivityManager.h"
#include "homeassistant/HaConfigStore.h"
#include "network/WifiJoin.h"
#ifndef SIMULATOR
#include "network/WifiPowerSaveGuard.h"
#endif

HaLink haLink;

namespace {

constexpr unsigned long kJoinTimeoutMs = 6000;
constexpr uint16_t kHttpTimeoutMs = 5000;
// HTTPClient, a JSON body and ArduinoJson parsing of one state object.
constexpr uint32_t kWorkerStackBytes = 8192;

std::mutex mtx;
ha::IdlePolicy policy;
ha::LinkStatus lastStatus = ha::LinkStatus::Idle;
bool inFlight = false;
bool haveResult = false;
std::string resultEntity;
ha::EntityState resultState;
uint32_t generation = 0;  // bumped on sleep; a worker from before drops its result

// GET or POST <url><path>; returns the HTTP code (<= 0 on failure) and the body.
int request(const char* method, const std::string& path, const std::string& body, std::string* response) {
  NetworkClient client;
  HTTPClient http;
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setTimeout(kHttpTimeoutMs);
  const std::string url = ha::config().url + path;
  http.begin(client, url.c_str());
  http.addHeader("Authorization", (std::string("Bearer ") + ha::bearerToken()).c_str());
  int code;
  if (method[0] == 'P') {
    http.addHeader("Content-Type", "application/json");
    code = http.POST(body.c_str());
  } else {
    code = http.GET();
  }
  if (response && code > 0) *response = http.getString().c_str();
  http.end();
  return code;
}

ha::LinkStatus runCall(const ha::ServiceCall& call, std::string& entityId, ha::EntityState& fresh) {
  if (!haLink.ensureHomeNetwork()) return ha::LinkStatus::NoNetwork;
#ifndef SIMULATOR
  WifiPowerSaveGuard powerSave;
#endif
  const int code = request("POST", "/api/services/" + call.domain + "/" + call.service, ha::serviceBody(call), nullptr);
  const ha::LinkStatus status = ha::statusFromHttp(code);
  LOG_INF("HA", "HA call %s.%s %s -> %d", call.domain.c_str(), call.service.c_str(), call.entityId.c_str(), code);
  if (status != ha::LinkStatus::Ok) return status;
  std::string json;
  const int getCode = request("GET", "/api/states/" + call.entityId, "", &json);
  if (getCode == 200 && ha::parseStateObject(json, entityId, fresh)) return ha::LinkStatus::Ok;
  if (getCode == 404) {
    entityId = call.entityId;
    fresh = ha::EntityState{"unavailable", "", "", -1, "", ""};
    return ha::LinkStatus::Ok;
  }
  return ha::statusFromHttp(getCode);
}

struct Job {
  ha::ServiceCall call;
  uint32_t generation;
};

void worker(void* arg) {
  Job* job = static_cast<Job*>(arg);
  std::string entityId;
  ha::EntityState fresh;
  const ha::LinkStatus status = runCall(job->call, entityId, fresh);
  LOG_DBG("HA", "HA worker stack left: %u bytes", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  {
    std::lock_guard<std::mutex> lock(mtx);
    if (job->generation == generation) {
      lastStatus = status;
      if (status == ha::LinkStatus::Ok && !entityId.empty()) {
        haveResult = true;
        resultEntity = entityId;
        resultState = fresh;
      }
      policy.noteActivity(millis());
    }
    inFlight = false;
  }
  delete job;  // the screen that submitted polls busy()/takeResult() from its loop
  vTaskDelete(nullptr);
}

}  // namespace

bool HaLink::submit(const ha::ServiceCall& call) {
  if (!ha::enabled()) return false;
  std::lock_guard<std::mutex> lock(mtx);
  if (inFlight) return false;
  auto* job = new Job{call, generation};
  inFlight = true;
  lastStatus = ha::LinkStatus::Working;
  if (xTaskCreate(worker, "HaLink", kWorkerStackBytes, job, 1, nullptr) != pdTRUE) {
    LOG_ERR("HA", "HA worker could not start");
    delete job;
    inFlight = false;
    lastStatus = ha::LinkStatus::NoAnswer;
    return false;
  }
  return true;
}

bool HaLink::busy() const {
  std::lock_guard<std::mutex> lock(mtx);
  return inFlight;
}

ha::LinkStatus HaLink::status() const {
  std::lock_guard<std::mutex> lock(mtx);
  return lastStatus;
}

bool HaLink::takeResult(std::string& entityId, ha::EntityState& fresh) {
  std::lock_guard<std::mutex> lock(mtx);
  if (!haveResult) return false;
  haveResult = false;
  entityId = std::move(resultEntity);
  fresh = std::move(resultState);
  return true;
}

bool HaLink::ensureHomeNetwork() {
  const bool wasUp = WiFi.status() == WL_CONNECTED;
  if (!joinWifiNetwork(ha::config().wifiSsid, kJoinTimeoutMs)) {
    LOG_INF("HA", "HA: home network not available");
    return false;
  }
  std::lock_guard<std::mutex> lock(mtx);
  policy.noteJoined(wasUp, millis());
  return true;
}

void HaLink::noteActivity() {
  std::lock_guard<std::mutex> lock(mtx);
  policy.noteActivity(millis());
}

void HaLink::tick() {
  bool release;
  {
    std::lock_guard<std::mutex> lock(mtx);
    if (inFlight || !policy.ownsWifi()) return;
    release = policy.shouldRelease(millis(), SETTINGS.developerMode != 0, activityManager.allowsHomeAssistantWifiOff());
  }
  if (!release) return;
  LOG_INF("HA", "HA: Wi-Fi idle, turning it off");
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
}

void HaLink::onSleep() {
  std::lock_guard<std::mutex> lock(mtx);
  policy.noteSleep();
  ++generation;
  haveResult = false;
  lastStatus = ha::LinkStatus::Idle;
}
