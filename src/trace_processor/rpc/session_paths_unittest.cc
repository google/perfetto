/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/rpc/session_paths.h"

#include <string>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::session {
namespace {

TEST(SessionPathsTest, IsValidSessionName) {
  EXPECT_TRUE(IsValidSessionName("calm-blue-otter"));
  EXPECT_TRUE(IsValidSessionName("a"));
  EXPECT_TRUE(IsValidSessionName("Trace_1"));
  EXPECT_TRUE(IsValidSessionName("9lives"));

  EXPECT_FALSE(IsValidSessionName(""));
  EXPECT_FALSE(IsValidSessionName("-leading-dash"));
  EXPECT_FALSE(IsValidSessionName("_leading_underscore"));
  EXPECT_FALSE(IsValidSessionName("has space"));
  EXPECT_FALSE(IsValidSessionName("has/slash"));
  EXPECT_FALSE(IsValidSessionName("has.dot"));
  EXPECT_FALSE(IsValidSessionName("has:colon"));
  EXPECT_FALSE(IsValidSessionName(std::string(65, 'a')));
}

TEST(SessionPathsTest, GenerateSessionNameIsValid) {
  // Generated names must always satisfy the name charset.
  for (int i = 0; i < 16; ++i) {
    std::string name = GenerateSessionName();
    EXPECT_TRUE(IsValidSessionName(name)) << name;
  }
}

TEST(SessionPathsTest, ParseDurationMs) {
  EXPECT_EQ(ParseDurationMs("30").value(), 30u * 1000);  // bare = seconds.
  EXPECT_EQ(ParseDurationMs("45s").value(), 45u * 1000);
  EXPECT_EQ(ParseDurationMs("5m").value(), 5u * 60 * 1000);
  EXPECT_EQ(ParseDurationMs("2h").value(), 2u * 3600 * 1000);
  EXPECT_EQ(ParseDurationMs("0").value(), 0u);
  EXPECT_EQ(ParseDurationMs("never").value(), 0u);
  EXPECT_EQ(ParseDurationMs("off").value(), 0u);

  EXPECT_FALSE(ParseDurationMs("").ok());
  EXPECT_FALSE(ParseDurationMs("abc").ok());
  EXPECT_FALSE(ParseDurationMs("-5").ok());
  EXPECT_FALSE(ParseDurationMs("5x").ok());
}

TEST(SessionPathsTest, ClassifyRemoteAddr) {
  EXPECT_EQ(ClassifyRemoteAddr("calm-blue-otter"),
            RemoteAddrKind::kSessionName);
  EXPECT_EQ(ClassifyRemoteAddr("mysession"), RemoteAddrKind::kSessionName);

  EXPECT_EQ(ClassifyRemoteAddr("/run/user/1000/perfetto/x.sock"),
            RemoteAddrKind::kUnixPath);
  EXPECT_EQ(ClassifyRemoteAddr("relative.sock"), RemoteAddrKind::kUnixPath);
  EXPECT_EQ(ClassifyRemoteAddr("/abs/path"), RemoteAddrKind::kUnixPath);
  EXPECT_EQ(ClassifyRemoteAddr("C:\\tmp\\x.sock"), RemoteAddrKind::kUnixPath);

  EXPECT_EQ(ClassifyRemoteAddr("localhost:9001"), RemoteAddrKind::kHttp);
  EXPECT_EQ(ClassifyRemoteAddr("http://localhost:9001"), RemoteAddrKind::kHttp);
  EXPECT_EQ(ClassifyRemoteAddr("127.0.0.1:9001"), RemoteAddrKind::kHttp);
}

TEST(SessionPathsTest, ValidateAfUnixPathLength) {
  EXPECT_TRUE(ValidateAfUnixPathLength("/tmp/perfetto/x.sock").ok());
  EXPECT_FALSE(ValidateAfUnixPathLength(std::string(200, 'a')).ok());
}

}  // namespace
}  // namespace perfetto::trace_processor::session
