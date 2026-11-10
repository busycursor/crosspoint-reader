#pragma once

#include <HaIdlePolicy.h>
#include <HaState.h>

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// The Control page's connection to Home Assistant while it is open: joins the
// home network, fetches the dashboard layout, and follows its entities' states.
// Runs on its own worker task; the page polls it from loop(). On the device it
// uses HA's WebSocket API (live updates); the simulator polls REST instead.
class HaSession {
 public:
  struct Update {
    std::string layoutJson;  // non-empty: a fresh Lovelace config
    std::vector<std::pair<std::string, ha::EntityState>> changes;
  };

  virtual ~HaSession() = default;
  // Starts the worker. The session keeps itself alive until the worker ends.
  static std::shared_ptr<HaSession> start();
  // Asks the worker to end; returns at once.
  void stop();
  // Drains what arrived since the last call; false when nothing did.
  bool poll(Update& out);
  ha::LinkStatus status() const;

 protected:
  virtual void run() = 0;  // returns when stopRequested() or on a fatal error
  bool stopRequested() const;
  void setStatus(ha::LinkStatus s);
  void pushLayout(std::string json);
  void pushChange(const std::string& entityId, const ha::EntityState& s);
  // Entities the dashboard shows; set once the layout is known.
  std::vector<std::string> entities;
  std::vector<std::string> weatherEntities;
  void setLayoutEntities(const std::string& layoutJson);

 private:
  static void worker(void* arg);
  mutable std::mutex mtx_;
  bool stop_ = false;
  ha::LinkStatus status_ = ha::LinkStatus::Working;
  Update pending_;
};
