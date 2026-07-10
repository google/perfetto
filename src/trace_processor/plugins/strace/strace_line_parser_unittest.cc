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

#include "src/trace_processor/plugins/strace/strace_line_parser.h"

#include <optional>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::strace_importer {
namespace {

TEST(StraceLineParserTest, CompleteCall) {
  auto line = ParseStraceLine(
      R"(14:32:01 openat(AT_FDCWD, "/etc/passwd", O_RDONLY) = 3)");
  ASSERT_TRUE(line.has_value());
  EXPECT_EQ(line->tod_ns, (14 * 3600 + 32 * 60 + 1) * 1000LL * 1000 * 1000);
  EXPECT_FALSE(line->pid.has_value());
  EXPECT_EQ(line->syscall, "openat");
  EXPECT_EQ(line->args, R"(AT_FDCWD, "/etc/passwd", O_RDONLY)");
  ASSERT_TRUE(line->return_value.has_value());
  EXPECT_EQ(*line->return_value, "3");
  EXPECT_FALSE(line->is_unfinished);
  EXPECT_FALSE(line->is_resumed);
}

TEST(StraceLineParserTest, MicrosecondTimestamp) {
  auto line = ParseStraceLine(R"(14:32:01.123456 read(3, "abc", 1024) = 3)");
  ASSERT_TRUE(line.has_value());
  EXPECT_EQ(line->tod_ns,
            (14 * 3600 + 32 * 60 + 1) * 1000LL * 1000 * 1000 + 123456000);
}

TEST(StraceLineParserTest, PidPrefixFromDashF) {
  auto line = ParseStraceLine(R"(1234 14:32:01 read(3, "abc", 1024) = 3)");
  ASSERT_TRUE(line.has_value());
  ASSERT_TRUE(line->pid.has_value());
  EXPECT_EQ(*line->pid, 1234u);
  EXPECT_EQ(line->syscall, "read");
}

TEST(StraceLineParserTest, Unfinished) {
  auto line = ParseStraceLine(R"(14:32:01 read(3,  <unfinished ...>)");
  ASSERT_TRUE(line.has_value());
  EXPECT_EQ(line->syscall, "read");
  EXPECT_TRUE(line->is_unfinished);
  EXPECT_FALSE(line->is_resumed);
  EXPECT_FALSE(line->return_value.has_value());
}

TEST(StraceLineParserTest, Resumed) {
  auto line =
      ParseStraceLine(R"(14:32:01 <... read resumed> "abc", 1024) = 3)");
  ASSERT_TRUE(line.has_value());
  EXPECT_EQ(line->syscall, "read");
  EXPECT_FALSE(line->is_unfinished);
  EXPECT_TRUE(line->is_resumed);
  ASSERT_TRUE(line->return_value.has_value());
  EXPECT_EQ(*line->return_value, "3");
}

TEST(StraceLineParserTest, ErrorReturnValue) {
  auto line = ParseStraceLine(
      R"(14:32:01 openat(AT_FDCWD, "/nope", O_RDONLY) = -1 ENOENT (No such file or directory))");
  ASSERT_TRUE(line.has_value());
  ASSERT_TRUE(line->return_value.has_value());
  EXPECT_EQ(*line->return_value, "-1 ENOENT (No such file or directory)");
}

TEST(StraceLineParserTest, SkipsSignalDeliveryLine) {
  EXPECT_FALSE(
      ParseStraceLine(R"(--- SIGCHLD {si_signo=SIGCHLD} ---)").has_value());
}

TEST(StraceLineParserTest, SkipsExitBanner) {
  EXPECT_FALSE(ParseStraceLine("+++ exited with 0 +++").has_value());
}

TEST(StraceLineParserTest, RejectsEmptyLine) {
  EXPECT_FALSE(ParseStraceLine("").has_value());
  EXPECT_FALSE(ParseStraceLine("   ").has_value());
}

TEST(StraceLineParserTest, RejectsLineWithoutTimestamp) {
  EXPECT_FALSE(
      ParseStraceLine(R"(openat(AT_FDCWD, "/etc/passwd") = 3)").has_value());
}

TEST(StraceLineParserTest, IsStraceFormatTraceSniffing) {
  std::string trace =
      "14:32:01 openat(AT_FDCWD, \"/etc/passwd\", O_RDONLY) = 3\n"
      "14:32:01 read(3, \"root\"..., 1024) = 4\n";
  EXPECT_TRUE(IsStraceFormatTrace(
      reinterpret_cast<const uint8_t*>(trace.data()), trace.size()));

  std::string not_strace = "{\"traceEvents\": []}";
  EXPECT_FALSE(IsStraceFormatTrace(
      reinterpret_cast<const uint8_t*>(not_strace.data()), not_strace.size()));
}

}  // namespace
}  // namespace perfetto::trace_processor::strace_importer
