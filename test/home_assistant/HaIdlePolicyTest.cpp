#include <gtest/gtest.h>

#include "HaIdlePolicy.h"

using ha::IdlePolicy;
using ha::LinkStatus;

TEST(HaLinkStatus, Http401IsTokenRejected) {
  EXPECT_EQ(ha::statusFromHttp(401), LinkStatus::TokenRejected);
  EXPECT_EQ(ha::statusFromHttp(403), LinkStatus::TokenRejected);
}

TEST(HaLinkStatus, Http404KeepsLinkOk) {
  EXPECT_EQ(ha::statusFromHttp(200), LinkStatus::Ok);
  EXPECT_EQ(ha::statusFromHttp(404), LinkStatus::Ok);
  EXPECT_EQ(ha::statusFromHttp(400), LinkStatus::Ok);
}

TEST(HaLinkStatus, TimeoutIsNoAnswer) {
  EXPECT_EQ(ha::statusFromHttp(-1), LinkStatus::NoAnswer);
  EXPECT_EQ(ha::statusFromHttp(0), LinkStatus::NoAnswer);
  EXPECT_EQ(ha::statusFromHttp(502), LinkStatus::NoAnswer);
}

TEST(HaIdlePolicy, ReleasesAfterTwoMinutes) {
  IdlePolicy p;
  p.noteJoined(/*wifiWasAlreadyUp=*/false, 1000);
  EXPECT_FALSE(p.shouldRelease(1000 + 119999, false, true));
  EXPECT_TRUE(p.shouldRelease(1000 + 120000, false, true));
  EXPECT_FALSE(p.shouldRelease(1000 + 130000, false, true));  // once
}

TEST(HaIdlePolicy, ActivityResetsTimer) {
  IdlePolicy p;
  p.noteJoined(false, 0);
  p.noteActivity(100000);
  EXPECT_FALSE(p.shouldRelease(150000, false, true));
  EXPECT_TRUE(p.shouldRelease(220000, false, true));
}

TEST(HaIdlePolicy, NeverReleasesWifiItDidNotStart) {
  IdlePolicy p;
  p.noteJoined(/*wifiWasAlreadyUp=*/true, 0);
  EXPECT_FALSE(p.shouldRelease(10 * 60 * 1000, false, true));
  IdlePolicy fresh;
  EXPECT_FALSE(fresh.shouldRelease(10 * 60 * 1000, false, true));
}

TEST(HaIdlePolicy, NeverDuringDeveloperMode) {
  IdlePolicy p;
  p.noteJoined(false, 0);
  EXPECT_FALSE(p.shouldRelease(200000, /*developerMode=*/true, true));
  EXPECT_TRUE(p.shouldRelease(200001, false, true));
}

TEST(HaIdlePolicy, WaitsForAllowingActivity) {
  IdlePolicy p;
  p.noteJoined(false, 0);
  EXPECT_FALSE(p.shouldRelease(200000, false, /*activityAllows=*/false));
  EXPECT_TRUE(p.shouldRelease(300000, false, true));
}

TEST(HaIdlePolicy, SleepResetsOwnership) {
  IdlePolicy p;
  p.noteJoined(false, 0);
  p.noteSleep();
  EXPECT_FALSE(p.shouldRelease(200000, false, true));
  EXPECT_FALSE(p.ownsWifi());
}

TEST(HaIdlePolicy, WrapsAroundMillis) {
  IdlePolicy p;
  p.noteJoined(false, 0xFFFFFF00u);
  EXPECT_FALSE(p.shouldRelease(0x00000100u, false, true));
  EXPECT_TRUE(p.shouldRelease(0xFFFFFF00u + 120000u, false, true));
}
