#include <gtest/gtest.h>

#include "HaDashboard.h"

namespace {

ha::Dashboard parse(const std::string& json, size_t view = 0) {
  ha::Dashboard d;
  EXPECT_TRUE(ha::parseDashboard(json, view, d));
  return d;
}

std::string sectionsView(const std::string& sections) {
  return R"({"title":"E-reader","views":[{"title":"Home","type":"sections","sections":[)" + sections + "]}]}";
}

}  // namespace

TEST(HaDashboard, SectionsViewWithHeadings) {
  const auto d = parse(sectionsView(R"(
    {"type":"grid","cards":[
      {"type":"heading","heading":"Outside"},
      {"type":"tile","entity":"sensor.outdoor_temp"},
      {"type":"heading","heading":"Lights"},
      {"type":"tile","entity":"light.reading_lamp","name":"Lamp"}]},
    {"type":"grid","cards":[{"type":"tile","entity":"switch.fan"}]})"));
  EXPECT_EQ(d.title, "E-reader");
  ASSERT_EQ(d.sections.size(), 3u);
  EXPECT_EQ(d.sections[0].title, "Outside");
  ASSERT_EQ(d.sections[0].cards.size(), 1u);
  EXPECT_EQ(d.sections[0].cards[0].entity, "sensor.outdoor_temp");
  EXPECT_EQ(d.sections[1].title, "Lights");
  EXPECT_EQ(d.sections[1].cards[0].name, "Lamp");
  EXPECT_EQ(d.sections[2].title, "");
  EXPECT_EQ(d.sections[2].cards[0].entity, "switch.fan");
}

TEST(HaDashboard, MasonryViewIsOneSection) {
  const auto d = parse(R"({"views":[{"cards":[{"type":"entity","entity":"sensor.a"},{"type":"button","entity":"scene.b"}]}]})");
  ASSERT_EQ(d.sections.size(), 1u);
  EXPECT_EQ(d.sections[0].title, "");
  ASSERT_EQ(d.sections[0].cards.size(), 2u);
  EXPECT_EQ(d.sections[0].cards[1].entity, "scene.b");
  EXPECT_EQ(d.sections[0].cards[1].kind, ha::CardKind::Entity);
}

TEST(HaDashboard, FlattensNestedGridAndStacks) {
  const auto d = parse(R"({"views":[{"cards":[
    {"type":"tile","entity":"light.a"},
    {"type":"grid","cards":[
      {"type":"vertical-stack","cards":[
        {"type":"horizontal-stack","cards":[{"type":"tile","entity":"light.b"}]},
        {"type":"tile","entity":"light.c"}]}]},
    {"type":"tile","entity":"light.d"}]}]})");
  ASSERT_EQ(d.sections.size(), 1u);
  const auto& cards = d.sections[0].cards;
  ASSERT_EQ(cards.size(), 4u);
  EXPECT_EQ(cards[0].entity, "light.a");
  EXPECT_EQ(cards[1].entity, "light.b");
  EXPECT_EQ(cards[2].entity, "light.c");
  EXPECT_EQ(cards[3].entity, "light.d");
}

TEST(HaDashboard, EntitiesRowsBecomeCards) {
  const auto d = parse(R"({"views":[{"cards":[{"type":"entities","title":"Laundry","entities":[
    "sensor.oven_timer",
    {"type":"divider"},
    {"entity":"sensor.kettle_timer","name":"Kettle"},
    {"type":"section","label":"x"}]}]}]})");
  ASSERT_EQ(d.sections.size(), 1u);
  const auto& cards = d.sections[0].cards;
  ASSERT_EQ(cards.size(), 2u);
  EXPECT_EQ(cards[0].entity, "sensor.oven_timer");
  EXPECT_EQ(cards[1].entity, "sensor.kettle_timer");
  EXPECT_EQ(cards[1].name, "Kettle");
}

TEST(HaDashboard, EntitiesCardTitleStartsSection) {
  const auto d = parse(R"({"views":[{"cards":[{"type":"tile","entity":"light.a"},
    {"type":"entities","title":"Laundry","entities":["sensor.w"]}]}]})");
  ASSERT_EQ(d.sections.size(), 2u);
  EXPECT_EQ(d.sections[1].title, "Laundry");
}

TEST(HaDashboard, DropsUnsupportedCards) {
  const auto d = parse(sectionsView(R"(
    {"type":"grid","cards":[
      {"type":"heading","heading":"Gone"},
      {"type":"custom:mushroom-template-card","entity":"light.x"},
      {"type":"picture-entity","entity":"camera.door"},
      {"type":"markdown","content":"hi"}]},
    {"type":"grid","cards":[{"type":"heading","heading":"Kept"},{"type":"tile","entity":"light.y"},
      {"type":"button","tap_action":{"action":"navigate"}}]})"));
  ASSERT_EQ(d.sections.size(), 1u);
  EXPECT_EQ(d.sections[0].title, "Kept");
  ASSERT_EQ(d.sections[0].cards.size(), 1u);
}

TEST(HaDashboard, SliderOnlyForLightBrightness) {
  const auto d = parse(R"({"views":[{"cards":[
    {"type":"tile","entity":"light.a","features":[{"type":"light-brightness"}]},
    {"type":"tile","entity":"switch.b","features":[{"type":"light-brightness"}]},
    {"type":"tile","entity":"light.c"}]}]})");
  const auto& cards = d.sections[0].cards;
  ASSERT_EQ(cards.size(), 3u);
  EXPECT_TRUE(cards[0].slider);
  EXPECT_FALSE(cards[1].slider);
  EXPECT_FALSE(cards[2].slider);
}

TEST(HaDashboard, WeatherForecastCard) {
  const auto d = parse(R"({"views":[{"cards":[{"type":"weather-forecast","entity":"weather.home"}]}]})");
  ASSERT_EQ(d.sections[0].cards.size(), 1u);
  EXPECT_EQ(d.sections[0].cards[0].kind, ha::CardKind::Weather);
  EXPECT_EQ(d.sections[0].cards[0].entity, "weather.home");
}

TEST(HaDashboard, SecondView) {
  const auto d = parse(R"({"views":[{"cards":[{"type":"tile","entity":"light.a"}]},
    {"title":"Two","cards":[{"type":"tile","entity":"light.b"}]}]})",
                       1);
  EXPECT_EQ(d.sections[0].cards[0].entity, "light.b");
}

TEST(HaDashboard, MissingViewFails) {
  ha::Dashboard d;
  EXPECT_FALSE(ha::parseDashboard(R"({"views":[]})", 0, d));
  EXPECT_FALSE(ha::parseDashboard("not json", 0, d));
}

TEST(HaDashboard, EntitiesAreUnique) {
  const auto d = parse(R"({"views":[{"cards":[{"type":"tile","entity":"light.a"},{"type":"tile","entity":"light.b"},
    {"type":"entity","entity":"light.a"}]}]})");
  const auto entities = ha::dashboardEntities(d);
  ASSERT_EQ(entities.size(), 2u);
  EXPECT_EQ(entities[0], "light.a");
  EXPECT_EQ(entities[1], "light.b");
}
