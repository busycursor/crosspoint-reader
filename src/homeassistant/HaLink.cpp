#include "HaLink.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <HaLive.h>
#include <Logging.h>
#include <NetworkClient.h>
#include <WiFi.h>

#include <mutex>

#include "CrossPointSettings.h"
#include "activities/ActivityManager.h"
#include "homeassistant/HaConfigStore.h"
#include "network/WifiJoin.h"
#ifndef SIMULATOR
#include <ESPmDNS.h>

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

std::mutex joinMtx;  // the link worker and the Control session may both join
std::mutex hostMtx;
std::string cachedHost;  // resolved address of a ".local" host; cleared with Wi-Fi

constexpr uint32_t kMdnsTimeoutMs = 2000;

}  // namespace

bool ha::resolvedHost(std::string& host, uint16_t& port) {
  if (!splitUrl(config().url, host, port)) return false;
  constexpr const char* kLocal = ".local";
  if (host.size() <= 6 || host.compare(host.size() - 6, 6, kLocal) != 0) return true;
#ifdef SIMULATOR
  return true;  // the host OS resolves .local names
#else
  std::lock_guard<std::mutex> lock(hostMtx);
  if (!cachedHost.empty()) {
    host = cachedHost;
    return true;
  }
  const std::string name = host.substr(0, host.size() - 6);
  MDNS.begin("crosspoint-ha");
  const IPAddress ip = MDNS.queryHost(name.c_str(), kMdnsTimeoutMs);
  MDNS.end();
  if (ip == IPAddress(0, 0, 0, 0)) {
    LOG_ERR("HA", "HA: %s did not resolve over mDNS", host.c_str());
    return false;
  }
  cachedHost = ip.toString().c_str();
  LOG_INF("HA", "HA: %s is %s", host.c_str(), cachedHost.c_str());
  host = cachedHost;
  return true;
#endif
}

int ha::restRequest(const char* method, const std::string& path, const std::string& body, std::string* response) {
  NetworkClient client;
  HTTPClient http;
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setTimeout(kHttpTimeoutMs);
  std::string host;
  uint16_t port = 0;
  if (!ha::resolvedHost(host, port)) return -1;
  const std::string url = "http://" + host + ":" + std::to_string(port) + path;
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

namespace {

// The entity's current state; Ok with "unavailable" for an entity HA does not know.
ha::LinkStatus readState(const std::string& id, std::string& entityId, ha::EntityState& out) {
  std::string json;
  const int code = ha::restRequest("GET", "/api/states/" + id, "", &json);
  if (code == 200 && ha::parseStateObject(json, entityId, out)) return ha::LinkStatus::Ok;
  if (code == 404) {
    entityId = id;
    out = ha::EntityState{"unavailable", "", "", -1, "", ""};
    return ha::LinkStatus::Ok;
  }
  return code == 200 ? ha::LinkStatus::NoAnswer : ha::statusFromHttp(code);
}

// A device often reports its new state to HA a moment after the service call
// returns; read until the state moves (up to kSettleReads), so the screen shows
// the result instead of the state from before the tap.
constexpr int kSettleReads = 4;
constexpr unsigned long kSettleGapMs = 500;

ha::LinkStatus runCall(const ha::ServiceCall& call, std::string& entityId, ha::EntityState& fresh) {
  if (!haLink.ensureHomeNetwork()) return ha::LinkStatus::NoNetwork;
#ifndef SIMULATOR
  WifiPowerSaveGuard powerSave;
#endif
  // Scenes, scripts and buttons have no state to wait for.
  const bool waitForChange = ha::isToggleable(call.entityId);
  ha::EntityState before;
  if (waitForChange) {
    std::string id;
    const ha::LinkStatus s = readState(call.entityId, id, before);
    if (s != ha::LinkStatus::Ok) return s;
  }
  const int code =
      ha::restRequest("POST", "/api/services/" + call.domain + "/" + call.service, ha::serviceBody(call), nullptr);
  const ha::LinkStatus status = ha::statusFromHttp(code);
  LOG_INF("HA", "HA call %s.%s %s -> %d", call.domain.c_str(), call.service.c_str(), call.entityId.c_str(), code);
  if (status != ha::LinkStatus::Ok) return status;
  for (int read = 0;; ++read) {
    const ha::LinkStatus s = readState(call.entityId, entityId, fresh);
    if (s != ha::LinkStatus::Ok) return s;
    const bool moved = fresh.state != before.state || fresh.brightnessPct != before.brightnessPct;
    if (!waitForChange || moved || read + 1 >= kSettleReads) {
      if (waitForChange && !moved) LOG_INF("HA", "HA: %s did not change yet", call.entityId.c_str());
      return ha::LinkStatus::Ok;
    }
    delay(kSettleGapMs);
  }
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
  std::lock_guard<std::mutex> joinLock(joinMtx);
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
  std::lock_guard<std::mutex> lock(hostMtx);
  cachedHost.clear();
}

void HaLink::onSleep() {
  {
    std::lock_guard<std::mutex> hostLock(hostMtx);
    cachedHost.clear();
  }
  std::lock_guard<std::mutex> lock(mtx);
  policy.noteSleep();
  ++generation;
  haveResult = false;
  lastStatus = ha::LinkStatus::Idle;
}
