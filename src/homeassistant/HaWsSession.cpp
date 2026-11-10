// Device session: Home Assistant's WebSocket API. Auth, the dashboard's
// Lovelace config, subscribe_entities for its entities (compressed live
// states) and a daily forecast subscription per weather card.
#ifndef SIMULATOR

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HaLive.h>
#include <Logging.h>
#include <WebSocketsClient.h>

#include "homeassistant/HaConfigStore.h"
#include "homeassistant/HaLink.h"
#include "homeassistant/HaSession.h"

namespace {

constexpr int kLayoutId = 1;
constexpr int kEntitiesId = 2;
constexpr int kForecastBaseId = 10;  // + index into weatherEntities
// Keeps the Wi-Fi idle timer from expiring while the page is open.
constexpr unsigned long kActivityNoteMs = 30000;

class HaWsSession final : public HaSession {
  WebSocketsClient ws;
  ha::StateMap states;
  bool fatal = false;

  void send(JsonDocument& doc) {
    std::string out;
    serializeJson(doc, out);
    ws.sendTXT(out.c_str(), out.size());
  }

  void onText(const uint8_t* payload, size_t length) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, length, DeserializationOption::NestingLimit(20)) != DeserializationError::Ok) {
      LOG_ERR("HA", "HA ws: unreadable message (%u bytes)", static_cast<unsigned>(length));
      return;
    }
    const std::string_view type = doc["type"] | "";
    if (type == "auth_required") {
      JsonDocument auth;
      auth["type"] = "auth";
      auth["access_token"] = ha::bearerToken();
      send(auth);
    } else if (type == "auth_invalid") {
      LOG_ERR("HA", "HA ws: token rejected");
      setStatus(ha::LinkStatus::TokenRejected);
      fatal = true;
    } else if (type == "auth_ok") {
      setStatus(ha::LinkStatus::Ok);
      JsonDocument req;
      req["id"] = kLayoutId;
      req["type"] = "lovelace/config";
      req["url_path"] = ha::config().dashboard;
      send(req);
    } else if (type == "result" && doc["id"] == kLayoutId) {
      if (!(doc["success"] | false)) {
        LOG_ERR("HA", "HA ws: dashboard %s not found", ha::config().dashboard.c_str());
        return;
      }
      std::string layout;
      serializeJson(doc["result"], layout);
      setLayoutEntities(layout);
      pushLayout(std::move(layout));
      subscribe();
    } else if (type == "event" && doc["id"] == kEntitiesId) {
      std::string event;
      serializeJson(doc["event"], event);
      std::vector<std::string> changed;
      ha::applyEntitiesEvent(event, states, &changed);
      for (const auto& id : changed) pushChange(id, states[id]);
    } else if (type == "event") {
      const int idx = (doc["id"] | 0) - kForecastBaseId;
      if (idx < 0 || idx >= static_cast<int>(weatherEntities.size())) return;
      std::string forecast;
      serializeJson(doc["event"]["forecast"], forecast);
      auto& s = states[weatherEntities[idx]];
      const std::string line = ha::forecastLine(forecast, 3);
      if (line != s.forecast) {
        s.forecast = line;
        pushChange(weatherEntities[idx], s);
      }
    }
  }

  void subscribe() {
    JsonDocument req;
    req["id"] = kEntitiesId;
    req["type"] = "subscribe_entities";
    JsonArray ids = req["entity_ids"].to<JsonArray>();
    for (const auto& id : entities) ids.add(id);
    send(req);
    for (size_t i = 0; i < weatherEntities.size(); ++i) {
      JsonDocument f;
      f["id"] = kForecastBaseId + static_cast<int>(i);
      f["type"] = "weather/subscribe_forecast";
      f["entity_id"] = weatherEntities[i];
      f["forecast_type"] = "daily";
      send(f);
    }
  }

  void run() override {
    if (!haLink.ensureHomeNetwork()) {
      setStatus(ha::LinkStatus::NoNetwork);
      return;
    }
    std::string host;
    uint16_t port = 0;
    if (!ha::splitUrl(ha::config().url, host, port)) {
      setStatus(ha::LinkStatus::NoAnswer);
      return;
    }
    ws.onEvent([this](WStype_t type, uint8_t* payload, size_t length) {
      if (type == WStype_TEXT) onText(payload, length);
      if (type == WStype_DISCONNECTED && !fatal) setStatus(ha::LinkStatus::NoAnswer);
    });
    ws.setReconnectInterval(5000);
    ws.begin(host.c_str(), port, "/api/websocket");
    unsigned long lastNote = millis();
    while (!stopRequested() && !fatal) {
      ws.loop();
      if (millis() - lastNote >= kActivityNoteMs) {
        lastNote = millis();
        haLink.noteActivity();
      }
      delay(10);
    }
    ws.disconnect();
    haLink.noteActivity();
  }
};

}  // namespace

std::unique_ptr<HaSession> makePlatformHaSession() { return std::make_unique<HaWsSession>(); }

#endif  // !SIMULATOR
