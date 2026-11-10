// Simulator session: REST polling against scripts/fake_ha.py (the simulator
// has no WebSocket client). The layout comes from the fake's fixture route.
#ifdef SIMULATOR

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Logging.h>

#include "homeassistant/HaLink.h"
#include "homeassistant/HaSession.h"

namespace {

constexpr unsigned long kPollMs = 5000;

class HaRestSession final : public HaSession {
  ha::StateMap seen;

  void pollStates() {
    for (const auto& id : entities) {
      std::string json, entityId;
      ha::EntityState s;
      const int code = ha::restRequest("GET", "/api/states/" + id, "", &json);
      if (code == 404) s.state = "unavailable";
      if (code != 404) {
        const auto status = ha::statusFromHttp(code);
        if (status != ha::LinkStatus::Ok) {
          setStatus(status);
          return;
        }
        if (!ha::parseStateObject(json, entityId, s)) continue;
      }
      if (std::find(weatherEntities.begin(), weatherEntities.end(), id) != weatherEntities.end()) {
        std::string resp;
        if (ha::restRequest("POST", "/api/services/weather/get_forecasts?return_response",
                            R"({"entity_id":")" + id + R"(","type":"daily"})", &resp) == 200) {
          JsonDocument doc;
          if (deserializeJson(doc, resp) == DeserializationError::Ok) {
            std::string forecast;
            serializeJson(doc["service_response"][id]["forecast"], forecast);
            s.forecast = ha::forecastLine(forecast, 3);
          }
        }
      }
      auto it = seen.find(id);
      if (it == seen.end() || it->second.state != s.state || it->second.brightnessPct != s.brightnessPct ||
          it->second.name != s.name || it->second.forecast != s.forecast || it->second.temperature != s.temperature) {
        seen[id] = s;
        pushChange(id, s);
      }
    }
    setStatus(ha::LinkStatus::Ok);
  }

  void run() override {
    if (!haLink.ensureHomeNetwork()) {
      setStatus(ha::LinkStatus::NoNetwork);
      return;
    }
    std::string layout;
    const int code = ha::restRequest("GET", "/api/lovelace-fixture", "", &layout);
    if (code != 200) {
      setStatus(ha::statusFromHttp(code) == ha::LinkStatus::Ok ? ha::LinkStatus::NoAnswer : ha::statusFromHttp(code));
      return;
    }
    setLayoutEntities(layout);
    pushLayout(layout);
    unsigned long last = 0;
    bool first = true;
    while (!stopRequested()) {
      if (first || millis() - last >= kPollMs) {
        first = false;
        last = millis();
        pollStates();
        haLink.noteActivity();
      }
      delay(100);
    }
  }
};

}  // namespace

std::unique_ptr<HaSession> makePlatformHaSession() { return std::make_unique<HaRestSession>(); }

#endif  // SIMULATOR
