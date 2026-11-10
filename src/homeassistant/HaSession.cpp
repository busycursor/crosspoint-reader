#include "HaSession.h"

#include <Arduino.h>
#include <HaDashboard.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
// WebSocket client, ArduinoJson documents for events and the layout.
constexpr uint32_t kSessionStackBytes = 12288;
}  // namespace

std::unique_ptr<HaSession> makePlatformHaSession();  // HaWsSession.cpp / HaRestSession.cpp

std::shared_ptr<HaSession> HaSession::start() {
  std::shared_ptr<HaSession> session = makePlatformHaSession();
  auto* ref = new std::shared_ptr<HaSession>(session);
  if (xTaskCreate(&HaSession::worker, "HaSession", kSessionStackBytes, ref, 1, nullptr) != pdTRUE) {
    LOG_ERR("HA", "HA session worker could not start");
    delete ref;
    session->setStatus(ha::LinkStatus::NoAnswer);
  }
  return session;
}

void HaSession::worker(void* arg) {
  auto* ref = static_cast<std::shared_ptr<HaSession>*>(arg);
  (*ref)->run();
  LOG_DBG("HA", "HA session stack left: %u bytes", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  delete ref;  // may destroy the session when the page already let go
  vTaskDelete(nullptr);
}

void HaSession::stop() {
  std::lock_guard<std::mutex> lock(mtx_);
  stop_ = true;
}

bool HaSession::stopRequested() const {
  std::lock_guard<std::mutex> lock(mtx_);
  return stop_;
}

bool HaSession::poll(Update& out) {
  std::lock_guard<std::mutex> lock(mtx_);
  if (pending_.layoutJson.empty() && pending_.changes.empty()) return false;
  out = std::move(pending_);
  pending_ = Update{};
  return true;
}

ha::LinkStatus HaSession::status() const {
  std::lock_guard<std::mutex> lock(mtx_);
  return status_;
}

void HaSession::setStatus(const ha::LinkStatus s) {
  std::lock_guard<std::mutex> lock(mtx_);
  status_ = s;
}

void HaSession::pushLayout(std::string json) {
  std::lock_guard<std::mutex> lock(mtx_);
  pending_.layoutJson = std::move(json);
}

void HaSession::pushChange(const std::string& entityId, const ha::EntityState& s) {
  std::lock_guard<std::mutex> lock(mtx_);
  for (auto& c : pending_.changes) {
    if (c.first == entityId) {
      c.second = s;
      return;
    }
  }
  pending_.changes.emplace_back(entityId, s);
}

void HaSession::setLayoutEntities(const std::string& layoutJson) {
  entities.clear();
  weatherEntities.clear();
  ha::Dashboard d;
  if (!ha::parseDashboard(layoutJson, 0, d)) return;
  entities = ha::dashboardEntities(d);
  for (const auto& s : d.sections) {
    for (const auto& c : s.cards) {
      if (c.kind == ha::CardKind::Weather) weatherEntities.push_back(c.entity);
    }
  }
}
