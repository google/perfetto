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

#include "src/trace_processor/plugins/strace/strace_trace_tokenizer.h"

#include <cstdint>
#include <optional>
#include <string>

#include "src/trace_processor/plugins/strace/strace_event.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::strace_importer {
namespace {

TEST(StraceLineParserTest, CompleteCall) {
  auto line =
      ParseStraceLine(
          R"(1700000000.000000 openat(AT_FDCWD, "/etc/passwd", O_RDONLY) = 3)")
          .line;
  ASSERT_TRUE(line.has_value());
  EXPECT_EQ(line->epoch_ns, 1700000000LL * 1000 * 1000 * 1000);
  EXPECT_FALSE(line->pid.has_value());
  EXPECT_EQ(line->syscall, "openat");
  EXPECT_EQ(line->args, R"(AT_FDCWD, "/etc/passwd", O_RDONLY)");
  ASSERT_TRUE(line->return_value.has_value());
  EXPECT_EQ(*line->return_value, "3");
  EXPECT_EQ(line->kind, StraceEventKind::kComplete);
}

TEST(StraceLineParserTest, MicrosecondTimestamp) {
  auto line =
      ParseStraceLine(R"(1700000000.123456 read(3, "abc", 1024) = 3)").line;
  ASSERT_TRUE(line.has_value());
  EXPECT_EQ(line->epoch_ns, 1700000000LL * 1000 * 1000 * 1000 + 123456000);
}

TEST(StraceLineParserTest, IntegerTimestampNoFraction) {
  auto line = ParseStraceLine(R"(1700000000 read(3, "abc", 1024) = 3)").line;
  ASSERT_TRUE(line.has_value());
  EXPECT_EQ(line->epoch_ns, 1700000000LL * 1000 * 1000 * 1000);
}

TEST(StraceLineParserTest, PidPrefixFromDashF) {
  auto line =
      ParseStraceLine(R"(1234 1700000000.000000 read(3, "abc", 1024) = 3)")
          .line;
  ASSERT_TRUE(line.has_value());
  ASSERT_TRUE(line->pid.has_value());
  EXPECT_EQ(*line->pid, 1234u);
  EXPECT_EQ(line->syscall, "read");
}

TEST(StraceLineParserTest, RejectsNegativeTimestampAfterPidPrefix) {
  // The leading '-' guard in ParseStraceLine only inspects the first
  // character of the whole line (to reject "--- SIGCHLD ... ---"), which is
  // the pid here, not the timestamp. A negative timestamp must still be
  // rejected once the pid prefix is stripped, rather than silently accepted
  // as a valid (nonsensical, pre-epoch) point in time.
  EXPECT_FALSE(
      ParseStraceLine(R"(1234 -5 read(3, "abc", 1024) = 3)").line.has_value());
}

TEST(StraceLineParserTest, RejectsNegativeTimestampNoPidPrefix) {
  EXPECT_FALSE(
      ParseStraceLine(R"(-5 read(3, "abc", 1024) = 3)").line.has_value());
}

TEST(StraceLineParserTest, RejectsOverflowingTimestamp) {
  // A digit run long enough that `seconds * kNsPerSec` would overflow
  // int64_t (undefined behaviour) must be rejected rather than silently
  // saturated/overflowed.
  EXPECT_FALSE(
      ParseStraceLine(R"(99999999999999999999 read(3, "abc", 1024) = 3)")
          .line.has_value());
}

TEST(StraceLineParserTest, RejectsOverflowingFractionalPart) {
  EXPECT_FALSE(ParseStraceLine(R"(1700000000.99999999999999999999 read(3) = 3)")
                   .line.has_value());
}

TEST(StraceLineParserTest, RejectsNegativeFractionalPart) {
  // Malformed input with a '-' after the decimal point; the parser would
  // otherwise happily parse "-123456" as microseconds and move the
  // timestamp backwards.
  EXPECT_FALSE(ParseStraceLine(R"(1700000000.-123456 read(3, "abc", 1024) = 3)")
                   .line.has_value());
}

TEST(StraceLineParserTest, Unfinished) {
  auto line =
      ParseStraceLine(R"(1700000000.000000 read(3,  <unfinished ...>)").line;
  ASSERT_TRUE(line.has_value());
  EXPECT_EQ(line->syscall, "read");
  EXPECT_EQ(line->kind, StraceEventKind::kUnfinished);
  EXPECT_FALSE(line->return_value.has_value());
}

TEST(StraceLineParserTest, Resumed) {
  auto line = ParseStraceLine(
                  R"(1700000000.000000 <... read resumed> "abc", 1024) = 3)")
                  .line;
  ASSERT_TRUE(line.has_value());
  EXPECT_EQ(line->syscall, "read");
  EXPECT_EQ(line->kind, StraceEventKind::kResumed);
  ASSERT_TRUE(line->return_value.has_value());
  EXPECT_EQ(*line->return_value, "3");
}

TEST(StraceLineParserTest, ResumedWithNoTrailingArgs) {
  // "<... syscall resumed>) = ret" — the original call had no args left to
  // print, so the resumed line collapses straight to the return value.
  auto line =
      ParseStraceLine(R"(1700000000.000500 <... futex resumed>)  = 0)").line;
  ASSERT_TRUE(line.has_value());
  EXPECT_EQ(line->syscall, "futex");
  EXPECT_EQ(line->kind, StraceEventKind::kResumed);
  ASSERT_TRUE(line->return_value.has_value());
  EXPECT_EQ(*line->return_value, "0");
}

TEST(StraceLineParserTest, ResumedThenUnfinishedAgain) {
  // A call resumed and then immediately interrupted again on the same line
  // (e.g. nested signal delivery). It both ends the prior call and begins a
  // new interrupted one, so it is neither a plain resume nor a plain start.
  auto line =
      ParseStraceLine(
          R"(1700000000.000000 <... epoll_wait resumed> {}, 64, -1 <unfinished ...>)")
          .line;
  ASSERT_TRUE(line.has_value());
  EXPECT_EQ(line->syscall, "epoll_wait");
  EXPECT_EQ(line->kind, StraceEventKind::kResumedThenUnfinished);
  EXPECT_FALSE(line->return_value.has_value());
}

TEST(StraceLineParserTest, ErrorReturnValue) {
  auto line =
      ParseStraceLine(
          R"(1700000000.000000 openat(AT_FDCWD, "/nope", O_RDONLY) = -1 ENOENT (No such file or directory))")
          .line;
  ASSERT_TRUE(line.has_value());
  ASSERT_TRUE(line->return_value.has_value());
  EXPECT_EQ(*line->return_value, "-1 ENOENT (No such file or directory)");
}

TEST(StraceLineParserTest, ReturnValueContainingParens) {
  // The return-value parsing anchors on the literal ") = " marker rather
  // than the last ')' in the line, so a parenthesised errno description
  // after the return value doesn't get truncated or misparsed as part of
  // the call's argument list.
  auto line =
      ParseStraceLine(
          R"(1700000000.000000 connect(3, {sa_family=AF_INET}, 16) = -1 ECONNREFUSED (Connection refused))")
          .line;
  ASSERT_TRUE(line.has_value());
  EXPECT_EQ(line->args, "3, {sa_family=AF_INET}, 16");
  ASSERT_TRUE(line->return_value.has_value());
  EXPECT_EQ(*line->return_value, "-1 ECONNREFUSED (Connection refused)");
}

TEST(StraceLineParserTest, SkipsSignalDeliveryLine) {
  EXPECT_FALSE(ParseStraceLine(R"(--- SIGCHLD {si_signo=SIGCHLD} ---)")
                   .line.has_value());
}

TEST(StraceLineParserTest, SkipsExitBanner) {
  EXPECT_FALSE(ParseStraceLine("+++ exited with 0 +++").line.has_value());
}

TEST(StraceLineParserTest, RejectsEmptyLine) {
  EXPECT_FALSE(ParseStraceLine("").line.has_value());
  EXPECT_FALSE(ParseStraceLine("   ").line.has_value());
}

TEST(StraceLineParserTest, RejectsLineWithoutTimestamp) {
  EXPECT_FALSE(ParseStraceLine(R"(openat(AT_FDCWD, "/etc/passwd") = 3)")
                   .line.has_value());
}

TEST(StraceLineParserTest, RejectsDashTTimeOfDayTimestamp) {
  // `-t`/`-tt` print wall-clock time-of-day with no date ("HH:MM:SS[.ffffff]"),
  // which can't be safely treated as an absolute point in time. Only `-ttt`
  // (Unix epoch) is supported; see strace_trace_tokenizer.h. This case gets
  // its own `unsupported_timestamp_format` flag (and a dedicated stat) since
  // it's a syscall line, just in an unsupported format, unlike a generic
  // non-syscall line.
  auto dash_t = ParseStraceLine(
      R"(14:32:01 openat(AT_FDCWD, "/etc/passwd", O_RDONLY) = 3)");
  EXPECT_FALSE(dash_t.line.has_value());
  EXPECT_TRUE(dash_t.unsupported_timestamp_format);

  auto dash_tt = ParseStraceLine(R"(14:32:01.123456 read(3, "abc", 1024) = 3)");
  EXPECT_FALSE(dash_tt.line.has_value());
  EXPECT_TRUE(dash_tt.unsupported_timestamp_format);
}

TEST(StraceLineParserTest, NonSyscallLineIsNotUnsupportedTimestampFormat) {
  // A line that's simply not a syscall at all (as opposed to a `-t`/`-tt`
  // syscall line) must not be misclassified as the timestamp-format case.
  EXPECT_FALSE(ParseStraceLine(R"(--- SIGCHLD {si_signo=SIGCHLD} ---)")
                   .unsupported_timestamp_format);
}

TEST(StraceLineParserTest, IsStraceFormatTraceSniffing) {
  std::string trace =
      "1700000000.000000 openat(AT_FDCWD, \"/etc/passwd\", O_RDONLY) = 3\n"
      "1700000000.000100 read(3, \"root\"..., 1024) = 4\n";
  EXPECT_TRUE(IsStraceFormatTrace(
      reinterpret_cast<const uint8_t*>(trace.data()), trace.size()));

  std::string not_strace = "{\"traceEvents\": []}";
  EXPECT_FALSE(IsStraceFormatTrace(
      reinterpret_cast<const uint8_t*>(not_strace.data()), not_strace.size()));

  // A `-t`/`-tt` (time-of-day) trace doesn't sniff as strace format either,
  // since it's rejected by the same timestamp parsing as above.
  std::string dash_t_trace =
      "14:32:01 openat(AT_FDCWD, \"/etc/passwd\", O_RDONLY) = 3\n";
  EXPECT_FALSE(
      IsStraceFormatTrace(reinterpret_cast<const uint8_t*>(dash_t_trace.data()),
                          dash_t_trace.size()));
}

}  // namespace
}  // namespace perfetto::trace_processor::strace_importer
