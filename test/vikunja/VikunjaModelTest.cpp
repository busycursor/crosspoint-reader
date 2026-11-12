#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

#include "VikunjaModel.h"

namespace {

std::string fixture(const char* name) {
  std::ifstream in(std::string(VIKUNJA_FIXTURES) + "/" + name);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

const char* kEgg = "Eggs \xF0\x9F\xA5\x9A";

}  // namespace

TEST(VikunjaModel, ParsesProjectsFixture) {
  std::vector<vikunja::Project> projects;
  ASSERT_TRUE(vikunja::appendProjectsPage(fixture("projects.json"), projects));
  ASSERT_EQ(projects.size(), 3u);
  EXPECT_EQ(projects[0].id, 10);
  EXPECT_EQ(projects[0].title, "Household");
  EXPECT_EQ(projects[0].parentId, 0);
  EXPECT_EQ(projects[0].listViewId, 101);
  EXPECT_EQ(projects[0].tableViewId, 103);
  EXPECT_FALSE(projects[0].archived);
  EXPECT_EQ(projects[1].title, "Groceries");
  EXPECT_EQ(projects[1].parentId, 10);
  EXPECT_TRUE(projects[2].archived);
}

TEST(VikunjaModel, ParsesTasksFixture) {
  vikunja::TaskList tasks;
  size_t count = 0;
  ASSERT_TRUE(vikunja::appendTasksPage(fixture("tasks_open.json"), tasks, &count));
  EXPECT_EQ(count, 2u);
  ASSERT_EQ(tasks.size(), 2u);
  EXPECT_GT(tasks.id(0), 0);
  EXPECT_EQ(tasks.title(0), "Milk");
  EXPECT_FALSE(tasks.done(0));

  vikunja::TaskList done;
  ASSERT_TRUE(vikunja::appendTasksPage(fixture("tasks_done.json"), done));
  ASSERT_EQ(done.size(), 3u);
  EXPECT_TRUE(done.done(0));
  EXPECT_EQ(done.title(2), kEgg);
}

TEST(VikunjaModel, AppendsSecondPage) {
  vikunja::TaskList tasks;
  ASSERT_TRUE(vikunja::appendTasksPage(fixture("tasks_open.json"), tasks));
  ASSERT_TRUE(vikunja::appendTasksPage(fixture("tasks_done.json"), tasks));
  EXPECT_EQ(tasks.size(), 5u);
  EXPECT_EQ(tasks.title(1), "Bread");
  EXPECT_EQ(tasks.title(2), "Milk");
}

TEST(VikunjaModel, EmptyPageIsFine) {
  vikunja::TaskList tasks;
  size_t count = 9;
  EXPECT_TRUE(vikunja::appendTasksPage("[]", tasks, &count));
  EXPECT_EQ(count, 0u);
  EXPECT_TRUE(vikunja::appendTasksPage("null", tasks));
  EXPECT_EQ(tasks.size(), 0u);
}

TEST(VikunjaModel, MalformedPageFails) {
  vikunja::TaskList tasks;
  EXPECT_FALSE(vikunja::appendTasksPage("[{\"id\":1", tasks));
  EXPECT_FALSE(vikunja::appendTasksPage("{\"message\":\"x\"}", tasks));
  std::vector<vikunja::Project> projects;
  EXPECT_FALSE(vikunja::appendProjectsPage("oops", projects));
}

TEST(VikunjaModel, FindTask) {
  vikunja::TaskList tasks;
  tasks.add(4, "a", false);
  tasks.add(9, "b", true);
  EXPECT_EQ(tasks.find(9), 1);
  EXPECT_EQ(tasks.find(5), -1);
}

TEST(VikunjaModel, SavedCopyRoundTrips) {
  std::vector<vikunja::Project> projects;
  ASSERT_TRUE(vikunja::appendProjectsPage(fixture("projects.json"), projects));
  std::vector<vikunja::Project> projectsBack;
  ASSERT_TRUE(vikunja::parseProjects(vikunja::serializeProjects(projects), projectsBack));
  EXPECT_EQ(projectsBack, projects);

  vikunja::TaskList tasks;
  ASSERT_TRUE(vikunja::appendTasksPage(fixture("tasks_open.json"), tasks));
  ASSERT_TRUE(vikunja::appendTasksPage(fixture("tasks_done.json"), tasks));
  vikunja::TaskList tasksBack;
  ASSERT_TRUE(vikunja::parseTasks(vikunja::serializeTasks(tasks), tasksBack));
  EXPECT_EQ(tasksBack, tasks);
}

TEST(VikunjaModel, SavedCopyEscapesTitles) {
  vikunja::TaskList tasks;
  tasks.add(7, kEgg, false);
  tasks.add(8, "tab\there\nnew \\ line", true);
  tasks.add(9, "", false);
  vikunja::TaskList back;
  ASSERT_TRUE(vikunja::parseTasks(vikunja::serializeTasks(tasks), back));
  ASSERT_EQ(back.size(), 3u);
  EXPECT_EQ(back.title(0), kEgg);
  EXPECT_EQ(back.title(1), "tab\there\nnew \\ line");
  EXPECT_TRUE(back.done(1));
  EXPECT_EQ(back.title(2), "");
}

TEST(VikunjaModel, SavedCopyDropsNul) {
  vikunja::TaskList tasks;
  tasks.add(7, std::string_view("Mi\0lk", 5), false);
  tasks.add(8, "Bread", false);
  const std::string text = vikunja::serializeTasks(tasks);
  EXPECT_EQ(text.find('\0'), std::string::npos);
  vikunja::TaskList back;
  ASSERT_TRUE(vikunja::parseTasks(text, back));
  ASSERT_EQ(back.size(), 2u);
  EXPECT_EQ(back.title(0), "Milk");
  EXPECT_EQ(back.title(1), "Bread");
}

TEST(VikunjaModel, SavedCopyRejectsGarbage) {
  vikunja::TaskList tasks;
  tasks.add(1, "keep", false);
  EXPECT_FALSE(vikunja::parseTasks("abc\t0\tx\n", tasks));
  EXPECT_FALSE(vikunja::parseTasks("12\t2\tx\n", tasks));
  EXPECT_FALSE(vikunja::parseTasks("12 0 x\n", tasks));
  EXPECT_EQ(tasks.size(), 1u);  // unchanged on failure
  EXPECT_TRUE(vikunja::parseTasks("", tasks));
  EXPECT_EQ(tasks.size(), 0u);
}

TEST(VikunjaModel, ReadsStar) {
  std::vector<vikunja::Project> projects;
  ASSERT_TRUE(vikunja::appendProjectsPage(
      R"([{"id":3,"title":"A","is_favorite":false},{"id":4,"title":"B","is_favorite":true}])", projects));
  ASSERT_EQ(projects.size(), 2u);
  EXPECT_FALSE(projects[0].starred);
  EXPECT_TRUE(projects[1].starred);
  std::vector<vikunja::Project> back;
  ASSERT_TRUE(vikunja::parseProjects(vikunja::serializeProjects(projects), back));
  EXPECT_EQ(back, projects);
}

TEST(VikunjaModel, FavouriteProject) {
  std::vector<vikunja::Project> projects(4);
  projects[0].id = 1;
  projects[1].id = 2;
  projects[1].starred = true;
  projects[1].archived = true;  // starred but archived: skipped
  projects[2].id = 3;
  projects[2].starred = true;
  projects[3].id = 4;
  projects[3].starred = true;
  EXPECT_EQ(vikunja::favouriteProject(projects, 0), 3);   // first starred
  EXPECT_EQ(vikunja::favouriteProject(projects, 1), 1);   // the device choice wins
  EXPECT_EQ(vikunja::favouriteProject(projects, 99), 3);  // a gone choice falls back
  for (auto& p : projects) p.starred = false;
  EXPECT_EQ(vikunja::favouriteProject(projects, 0), 0);
}

TEST(VikunjaModel, FindProject) {
  std::vector<vikunja::Project> projects;
  ASSERT_TRUE(vikunja::appendProjectsPage(fixture("projects.json"), projects));
  ASSERT_NE(vikunja::findProject(projects, 11), nullptr);
  EXPECT_EQ(vikunja::findProject(projects, 11)->title, "Groceries");
  EXPECT_EQ(vikunja::findProject(projects, 12345), nullptr);
}

TEST(VikunjaModel, SetDoneLeavesOtherBytes) {
  std::string json = R"({"id":5,"title":"say \"done\": true","position":0.001953125,"done" : false,)"
                     R"("related_tasks":{"blocks":[{"id":6,"done":false}]},"percent_done":0.1})";
  const std::string before = json;
  ASSERT_TRUE(vikunja::setTopLevelDone(json, true));
  std::string expected = before;
  expected.replace(expected.find("\"done\" : false") + 9, 5, "true");
  EXPECT_EQ(json, expected);
  EXPECT_NE(json.find("\"position\":0.001953125"), std::string::npos);
  EXPECT_NE(json.find("{\"id\":6,\"done\":false}"), std::string::npos);
}

TEST(VikunjaModel, SetDoneAfterNestedDone) {
  std::string json = R"({"related_tasks":{"x":[{"done":true}]},"done":true})";
  ASSERT_TRUE(vikunja::setTopLevelDone(json, false));
  EXPECT_EQ(json, R"({"related_tasks":{"x":[{"done":true}]},"done":false})");
}

TEST(VikunjaModel, SetDoneMissing) {
  std::string json = R"({"id":5,"title":"done"})";
  EXPECT_FALSE(vikunja::setTopLevelDone(json, true));
  EXPECT_EQ(json, R"({"id":5,"title":"done"})");
}

TEST(VikunjaModel, SetDoneOnRealTask) {
  const std::string page = fixture("tasks_open.json");
  // The fixture is an array; take its first object as the server's GET body.
  const size_t start = page.find('{');
  int depth = 0;
  size_t end = start;
  for (; end < page.size(); ++end) {
    if (page[end] == '{') ++depth;
    if (page[end] == '}' && --depth == 0) break;
  }
  std::string task = page.substr(start, end - start + 1);
  const std::string before = task;
  ASSERT_TRUE(vikunja::setTopLevelDone(task, true));
  EXPECT_EQ(task.size(), before.size() - 1);  // "false" -> "true"
  vikunja::TaskList parsed;
  ASSERT_TRUE(vikunja::appendTasksPage("[" + task + "]", parsed));
  ASSERT_EQ(parsed.size(), 1u);
  EXPECT_TRUE(parsed.done(0));
}
