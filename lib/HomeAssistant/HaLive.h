#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "HaState.h"

// Live updates for the Control page: HA's WebSocket subscribe_entities
// events, whose payload is compressed ("a" full states, "c" changes with
// "+" set and "-" removed attributes, "r" removed entities).
namespace ha {

// "http://host[:port]" → host and port (80 when absent).
bool splitUrl(const std::string& url, std::string& host, uint16_t& port);

// Applies one event payload (the "event" object) to m; changed gets each
// entity whose shown state differs afterwards. False for invalid JSON.
bool applyEntitiesEvent(std::string_view eventJson, StateMap& m, std::vector<std::string>* changed);

}  // namespace ha
