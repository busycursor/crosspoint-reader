#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// A Home Assistant dashboard (Lovelace config) reduced to what the Control
// page draws. Supported: heading, grid and stacks (flattened), tile, button,
// entity, entities rows and weather-forecast. Everything else is dropped.
namespace ha {

enum class CardKind : uint8_t { Entity, Weather };

struct Card {
  CardKind kind = CardKind::Entity;
  std::string entity;
  std::string name;     // empty: HA's friendly name
  bool slider = false;  // a light card with the light-brightness feature
};

struct Section {
  std::string title;  // from a heading card, or an entities card's title
  std::vector<Card> cards;
};

struct Dashboard {
  std::string title;
  std::vector<Section> sections;  // never empty sections
};

// view: index into "views". False for invalid JSON or a missing view.
bool parseDashboard(std::string_view lovelaceJson, size_t view, Dashboard& out);
// Every card's entity, without repeats, in order.
std::vector<std::string> dashboardEntities(const Dashboard& d);

}  // namespace ha
