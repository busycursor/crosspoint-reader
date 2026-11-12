#include <gtest/gtest.h>

#include "VikunjaStatus.h"

using vikunja::classifyResponse;
using vikunja::Status;

TEST(VikunjaStatus, Ok) {
  EXPECT_EQ(classifyResponse(200, false), Status::Ok);
  EXPECT_EQ(classifyResponse(204, false), Status::Ok);
}

TEST(VikunjaStatus, TokenRejected) {
  EXPECT_EQ(classifyResponse(401, false), Status::TokenRejected);
  EXPECT_EQ(classifyResponse(403, false), Status::TokenRejected);
}

TEST(VikunjaStatus, NoResponseIsOffline) { EXPECT_EQ(classifyResponse(0, false), Status::Offline); }

TEST(VikunjaStatus, ServerDown) {
  EXPECT_EQ(classifyResponse(500, false), Status::ServerDown);
  EXPECT_EQ(classifyResponse(530, false), Status::ServerDown);
  EXPECT_EQ(classifyResponse(404, false), Status::ServerDown);
}

TEST(VikunjaStatus, CloudflareLoginRedirect) {
  EXPECT_EQ(classifyResponse(302, false), Status::AccessRefused);
  EXPECT_EQ(classifyResponse(200, true), Status::AccessRefused);
  EXPECT_EQ(classifyResponse(401, true), Status::AccessRefused);
}
