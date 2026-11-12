#include <gtest/gtest.h>

#include "VikunjaPending.h"

namespace {

using vikunja::PendingTicks;
using vikunja::TaskList;

// Open 1, 2, 3 in list order; done 10, 11, most recent first.
TaskList saved() {
  TaskList t;
  t.add(1, "Milk", false);
  t.add(2, "Bread", false);
  t.add(3, "Eggs", false);
  t.add(10, "Coffee", true);
  t.add(11, "Apples", true);
  return t;
}

std::vector<int64_t> ids(const TaskList& list, const std::vector<size_t>& rows) {
  std::vector<int64_t> out;
  for (const size_t i : rows) out.push_back(list.id(i));
  return out;
}

}  // namespace

TEST(VikunjaPending, TickThenUntickCancels) {
  PendingTicks p;
  p.toggle(5, true, false);
  ASSERT_EQ(p.all().size(), 1u);
  p.toggle(5, false, false);
  EXPECT_TRUE(p.all().empty());
}

TEST(VikunjaPending, UntickDoneItemIsPending) {
  PendingTicks p;
  p.toggle(5, false, true);
  ASSERT_EQ(p.all().size(), 1u);
  EXPECT_EQ(p.all()[0].taskId, 5);
  EXPECT_FALSE(p.all()[0].done);
}

TEST(VikunjaPending, RetickMovesToEnd) {
  PendingTicks p;
  p.toggle(1, true, false);
  p.toggle(2, true, false);
  p.toggle(1, false, false);
  p.toggle(1, true, false);
  ASSERT_EQ(p.all().size(), 2u);
  EXPECT_EQ(p.all()[0].taskId, 2);
  EXPECT_EQ(p.all()[1].taskId, 1);
}

TEST(VikunjaPending, Remove) {
  PendingTicks p;
  p.toggle(1, true, false);
  p.toggle(2, true, false);
  p.remove(1);
  ASSERT_EQ(p.all().size(), 1u);
  EXPECT_EQ(p.all()[0].taskId, 2);
  p.remove(42);
  EXPECT_EQ(p.all().size(), 1u);
}

TEST(VikunjaPending, RoundTrip) {
  PendingTicks p;
  p.toggle(1, true, false, 7);
  p.toggle(9, false, true);
  PendingTicks back;
  ASSERT_TRUE(back.parse(p.serialize()));
  ASSERT_EQ(back.all().size(), 2u);
  EXPECT_EQ(back.all()[0].taskId, 1);
  EXPECT_TRUE(back.all()[0].done);
  EXPECT_EQ(back.all()[0].projectId, 7);
  EXPECT_EQ(back.all()[1].taskId, 9);
  EXPECT_EQ(back.all()[1].projectId, 0);
  EXPECT_FALSE(back.all()[1].done);
}

TEST(VikunjaPending, ParseRejectsGarbage) {
  PendingTicks p;
  p.toggle(1, true, false);
  EXPECT_FALSE(p.parse("{oops"));
  EXPECT_EQ(p.all().size(), 1u);  // unchanged on failure
  EXPECT_TRUE(p.parse("[]"));
  EXPECT_TRUE(p.all().empty());
}

TEST(VikunjaPending, SplitWithoutTicks) {
  const TaskList list = saved();
  std::vector<size_t> open, done;
  vikunja::splitTasks(list, PendingTicks{}, open, done);
  EXPECT_EQ(ids(list, open), (std::vector<int64_t>{1, 2, 3}));
  EXPECT_EQ(ids(list, done), (std::vector<int64_t>{10, 11}));
}

TEST(VikunjaPending, SplitAppliesTicks) {
  PendingTicks p;
  p.toggle(2, true, false);   // ticked first
  p.toggle(3, true, false);   // ticked last: top of done
  p.toggle(11, false, true);  // unticked: back to open, at the end
  const TaskList list = saved();
  std::vector<size_t> open, done;
  vikunja::splitTasks(list, p, open, done);
  EXPECT_EQ(ids(list, open), (std::vector<int64_t>{1, 11}));
  EXPECT_EQ(ids(list, done), (std::vector<int64_t>{3, 2, 10}));
}

TEST(VikunjaPending, SplitIgnoresTicksForUnknownTasks) {
  PendingTicks p;
  p.toggle(999, true, false);
  const TaskList list = saved();
  std::vector<size_t> open, done;
  vikunja::splitTasks(list, p, open, done);
  EXPECT_EQ(open.size(), 3u);
  EXPECT_EQ(done.size(), 2u);
}

TEST(VikunjaPending, SplitKeepsItemsWhoseTickTheServerMatches) {
  PendingTicks p;
  p.toggle(1, true, false);   // ticked offline...
  p.toggle(10, false, true);  // ...and unticked offline
  TaskList list;              // reload: the server already has both changes
  list.add(1, "Milk", true);
  list.add(10, "Coffee", false);
  std::vector<size_t> open, done;
  vikunja::splitTasks(list, p, open, done);
  EXPECT_EQ(ids(list, open), (std::vector<int64_t>{10}));
  EXPECT_EQ(ids(list, done), (std::vector<int64_t>{1}));
}

TEST(VikunjaPending, ApplySentUpdatesSavedCopy) {
  TaskList list = saved();
  EXPECT_TRUE(vikunja::applySent(list, 2, true));
  EXPECT_TRUE(list.done(1));
  EXPECT_TRUE(vikunja::applySent(list, 10, false));
  EXPECT_FALSE(list.done(3));
  EXPECT_FALSE(vikunja::applySent(list, 999, true));
}

TEST(VikunjaPending, ResolveDeletedDrops) {
  EXPECT_EQ(vikunja::resolveTick({1, true}, std::nullopt), vikunja::TickAction::Drop);
}

TEST(VikunjaPending, ResolveAlreadyInStateDrops) {
  EXPECT_EQ(vikunja::resolveTick({1, true}, true), vikunja::TickAction::Drop);
}

TEST(VikunjaPending, ResolveDiffersSends) {
  EXPECT_EQ(vikunja::resolveTick({1, true}, false), vikunja::TickAction::Send);
}
