#pragma once

#include <HaIdlePolicy.h>
#include <HaState.h>

#include <string>

// The Home Assistant connection for the reader's sheet and the Control page:
// joins the home network on demand (never by itself), sends service calls
// over REST on a short-lived worker task, and turns Wi-Fi off again after
// ha::kWifiIdleOffMs without HA activity (see ha::IdlePolicy).
class HaLink {
 public:
  // Starts a call on a worker; false while another call is in flight.
  bool submit(const ha::ServiceCall& call);
  bool busy() const;
  // Outcome of the last call (Idle before any).
  ha::LinkStatus status() const;
  // After a successful call: the entity's fresh state, once.
  bool takeResult(std::string& entityId, ha::EntityState& fresh);

  // For the Control page's own session: join (or reuse) the home network
  // from a worker. Blocks up to the join timeout.
  bool ensureHomeNetwork();
  void noteActivity();

  void tick();     // main loop: idle Wi-Fi off
  void onSleep();  // Wi-Fi is going down: forget ownership, drop results
};

extern HaLink haLink;

namespace ha {
// One REST request to the configured HA (bearer token set). Returns the HTTP
// code, <= 0 on failure; response gets the body when given.
int restRequest(const char* method, const std::string& path, const std::string& body, std::string* response);
// Host and port to connect to: the config's, with a ".local" host resolved
// over mDNS on the device (cached until Wi-Fi goes down). False when it does
// not resolve.
bool resolvedHost(std::string& host, uint16_t& port);
}  // namespace ha
