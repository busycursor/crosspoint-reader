#include "HaDashboard.h"

#include <ArduinoJson.h>

#include <algorithm>

namespace ha {
namespace {

// grid > vertical-stack > horizontal-stack is as deep as a sane layout goes.
constexpr int kMaxNesting = 3;

class Builder {
 public:
  explicit Builder(Dashboard& d) : d_(d) {}

  void startSection(std::string title) {
    dropEmptyTail();
    d_.sections.push_back(Section{std::move(title), {}});
  }

  void card(JsonObjectConst c, int depth) {
    const std::string_view type = c["type"] | "";
    if (type == "heading") {
      startSection(c["heading"] | "");
    } else if (type == "grid" || type == "vertical-stack" || type == "horizontal-stack") {
      if (depth >= kMaxNesting) return;
      for (JsonObjectConst child : c["cards"].as<JsonArrayConst>()) card(child, depth + 1);
    } else if (type == "tile" || type == "button" || type == "entity") {
      add(CardKind::Entity, c);
    } else if (type == "weather-forecast") {
      add(CardKind::Weather, c);
    } else if (type == "entities") {
      const char* title = c["title"];
      if (title && *title) startSection(title);
      for (JsonVariantConst row : c["entities"].as<JsonArrayConst>()) {
        if (row.is<const char*>()) {
          push(Card{CardKind::Entity, row.as<const char*>(), "", false});
        } else if (row.is<JsonObjectConst>() && row["type"].isNull()) {
          add(CardKind::Entity, row.as<JsonObjectConst>());
        }
      }
    }
  }

  void finish() { dropEmptyTail(); }

 private:
  void add(CardKind kind, JsonObjectConst c) {
    Card card{kind, c["entity"] | "", c["name"] | "", false};
    if (card.entity.empty()) return;
    if (kind == CardKind::Entity && card.entity.rfind("light.", 0) == 0) {
      for (JsonObjectConst f : c["features"].as<JsonArrayConst>()) {
        if (std::string_view(f["type"] | "") == "light-brightness") card.slider = true;
      }
    }
    push(std::move(card));
  }

  void push(Card card) {
    if (d_.sections.empty()) d_.sections.push_back(Section{});
    d_.sections.back().cards.push_back(std::move(card));
  }

  void dropEmptyTail() {
    if (!d_.sections.empty() && d_.sections.back().cards.empty()) d_.sections.pop_back();
  }

  Dashboard& d_;
};

}  // namespace

bool parseDashboard(std::string_view lovelaceJson, size_t view, Dashboard& out) {
  JsonDocument doc;
  // Views, sections, stacks and card features nest past ArduinoJson's default of 10.
  if (deserializeJson(doc, lovelaceJson.data(), lovelaceJson.size(), DeserializationOption::NestingLimit(20)) !=
      DeserializationError::Ok) {
    return false;
  }
  JsonArrayConst views = doc["views"].as<JsonArrayConst>();
  if (view >= views.size()) return false;
  JsonObjectConst v = views[view];

  Dashboard d;
  d.title = doc["title"] | "";
  Builder b(d);
  JsonArrayConst sections = v["sections"].as<JsonArrayConst>();
  if (!sections.isNull()) {
    for (JsonObjectConst s : sections) {
      b.startSection("");
      b.card(s, 0);  // a section is a grid card
    }
  } else {
    for (JsonObjectConst c : v["cards"].as<JsonArrayConst>()) b.card(c, 0);
  }
  b.finish();
  out = std::move(d);
  return true;
}

std::vector<std::string> dashboardEntities(const Dashboard& d) {
  std::vector<std::string> out;
  for (const auto& s : d.sections) {
    for (const auto& c : s.cards) {
      if (std::find(out.begin(), out.end(), c.entity) == out.end()) out.push_back(c.entity);
    }
  }
  return out;
}

}  // namespace ha
