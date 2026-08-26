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
  // A pid-less line must stay *parseable* (pid == nullopt) rather than
  // becoming a parse failure: format sniffing relies on it so that a trace
  // collected without `-f` is still recognised as strace and rejected with
  // the actionable strace_missing_pid stat, not a generic unknown-format
  // error.
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

TEST(StraceLineParserTest, BracketedPidPrefix) {
  // Writing to stderr (the default, and what a piped capture records)
  // strace prints the pid as a right-aligned "[pid  1234]" prefix rather
  // than the bare "1234 " form it uses with -o/-ff.
  auto line = ParseStraceLine(
                  R"([pid  1234] 1700000000.000000 read(3, "abc", 1024) = 3)")
                  .line;
  ASSERT_TRUE(line.has_value());
  ASSERT_TRUE(line->pid.has_value());
  EXPECT_EQ(*line->pid, 1234u);
  EXPECT_EQ(line->syscall, "read");
  EXPECT_EQ(line->args, R"(3, "abc", 1024)");
  ASSERT_TRUE(line->return_value.has_value());
  EXPECT_EQ(*line->return_value, "3");
}

TEST(StraceLineParserTest, BracketedPidPrefixOnResumedLine) {
  auto line =
      ParseStraceLine(
          R"([pid    75] 1700000000.000500 <... execve resumed>) = 0 <0.000328>)")
          .line;
  ASSERT_TRUE(line.has_value());
  ASSERT_TRUE(line->pid.has_value());
  EXPECT_EQ(*line->pid, 75u);
  EXPECT_EQ(line->syscall, "execve");
  EXPECT_EQ(line->kind, StraceEventKind::kResumed);
}

TEST(StraceLineParserTest, BracketedPidPrefixOnNonSyscallLine) {
  // Exit banners and signal deliveries carry the same pid prefix when
  // following processes. They are still not syscall lines, and must not be
  // mistaken for one (nor reported as a timestamp-format problem).
  auto exited =
      ParseStraceLine("[pid    75] 1700000000.000000 +++ exited with 0 +++");
  EXPECT_FALSE(exited.line.has_value());
  EXPECT_FALSE(exited.unsupported_timestamp_format);

  auto signalled = ParseStraceLine(
      "[pid    75] 1700000000.000000 --- SIGCHLD {si_signo=SIGCHLD} ---");
  EXPECT_FALSE(signalled.line.has_value());
  EXPECT_FALSE(signalled.unsupported_timestamp_format);
}

TEST(StraceLineParserTest, RejectsMalformedBracketedPid) {
  EXPECT_FALSE(ParseStraceLine(R"([pid  abc] 1700000000.000000 read(3) = 3)")
                   .line.has_value());
  EXPECT_FALSE(ParseStraceLine(R"([pid  12 1700000000.000000 read(3) = 3)")
                   .line.has_value());
}

TEST(StraceLineParserTest, StraceOwnDiagnosticLine) {
  // strace interleaves its own messages with the trace. These are not
  // syscall lines, and in particular the ':' in "strace:" must not make
  // them look like `-t`/`-tt` output: that would raise a spurious
  // "re-run with -ttt" error on a trace collected exactly as documented.
  for (const char* diagnostic :
       {"strace: Process 75 attached", "strace: Process 75 detached",
        "strace: [ Process PID=75 runs in 32 bit mode. ]"}) {
    auto result = ParseStraceLine(diagnostic);
    EXPECT_FALSE(result.line.has_value()) << diagnostic;
    EXPECT_FALSE(result.unsupported_timestamp_format) << diagnostic;
  }
}

TEST(StraceLineParserTest, SyscallDurationFromDashT) {
  auto line =
      ParseStraceLine(
          R"(1700000000.000000 clock_nanosleep(CLOCK_REALTIME, 0, {tv_sec=1}, NULL) = 0 <1.004879>)")
          .line;
  ASSERT_TRUE(line.has_value());
  ASSERT_TRUE(line->duration_ns.has_value());
  EXPECT_EQ(*line->duration_ns, 1004879000);
  // The suffix belongs to the duration, not to the return value.
  ASSERT_TRUE(line->return_value.has_value());
  EXPECT_EQ(*line->return_value, "0");
}

TEST(StraceLineParserTest, SyscallDurationScalesByDigitCount) {
  // Six digits is strace's default; `--syscall-times=ns` prints nine. The
  // fraction is scaled by how many digits are actually there, so neither is
  // silently off by a factor of 1000.
  auto us =
      ParseStraceLine(R"(1700000000.000000 close(3) = 0 <0.000123>)").line;
  ASSERT_TRUE(us.has_value());
  ASSERT_TRUE(us->duration_ns.has_value());
  EXPECT_EQ(*us->duration_ns, 123000);

  auto ns =
      ParseStraceLine(R"(1700000000.000000 close(3) = 0 <0.000004321>)").line;
  ASSERT_TRUE(ns.has_value());
  ASSERT_TRUE(ns->duration_ns.has_value());
  EXPECT_EQ(*ns->duration_ns, 4321);
}

TEST(StraceLineParserTest, DurationOnErrorReturnValue) {
  auto line =
      ParseStraceLine(
          R"(1700000000.000000 wait4(-1, 0x0, WNOHANG, NULL) = -1 ECHILD (No child processes) <0.000027>)")
          .line;
  ASSERT_TRUE(line.has_value());
  ASSERT_TRUE(line->duration_ns.has_value());
  EXPECT_EQ(*line->duration_ns, 27000);
  ASSERT_TRUE(line->return_value.has_value());
  EXPECT_EQ(*line->return_value, "-1 ECHILD (No child processes)");
}

TEST(StraceLineParserTest, NoDurationWithoutDashT) {
  auto line = ParseStraceLine(R"(1700000000.000000 close(3) = 0)").line;
  ASSERT_TRUE(line.has_value());
  EXPECT_FALSE(line->duration_ns.has_value());
  ASSERT_TRUE(line->return_value.has_value());
  EXPECT_EQ(*line->return_value, "0");
}

TEST(StraceLineParserTest, MalformedDurationStaysInReturnValue) {
  // Anything that isn't a well-formed duration is left alone rather than
  // silently dropped from the return value.
  auto line =
      ParseStraceLine(R"(1700000000.000000 fcntl(3, F_GETFL) = 0 <x>)").line;
  ASSERT_TRUE(line.has_value());
  EXPECT_FALSE(line->duration_ns.has_value());
  ASSERT_TRUE(line->return_value.has_value());
  EXPECT_EQ(*line->return_value, "0 <x>");
}

TEST(StraceLineParserTest, UnfinishedCallHasNoDuration) {
  auto line =
      ParseStraceLine(
          R"([pid    74] 1700000000.000000 wait4(-1,  <unfinished ...>)")
          .line;
  ASSERT_TRUE(line.has_value());
  EXPECT_EQ(line->kind, StraceEventKind::kUnfinished);
  EXPECT_FALSE(line->duration_ns.has_value());
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

TEST(StraceLineParserTest, ArgumentContainingClosingParenAndEquals) {
  // wait4 prints its status through macros, so the argument list itself
  // contains ") ==". Anchoring the return value on the first ")" followed
  // by "=" would cut the line in the middle of the arguments.
  auto line =
      ParseStraceLine(
          R"(1700000000.000000 wait4(-1, [{WIFEXITED(s) && WEXITSTATUS(s) == 0}], 0, NULL) = 75)")
          .line;
  ASSERT_TRUE(line.has_value());
  EXPECT_EQ(line->args, "-1, [{WIFEXITED(s) && WEXITSTATUS(s) == 0}], 0, NULL");
  ASSERT_TRUE(line->return_value.has_value());
  EXPECT_EQ(*line->return_value, "75");
}

TEST(StraceLineParserTest, StringArgumentContainingParenEquals) {
  auto line = ParseStraceLine(
                  R"(1700000000.000000 write(1, "x) = y", 6) = 6 <0.000004>)")
                  .line;
  ASSERT_TRUE(line.has_value());
  EXPECT_EQ(line->args, R"(1, "x) = y", 6)");
  ASSERT_TRUE(line->return_value.has_value());
  EXPECT_EQ(*line->return_value, "6");
  ASSERT_TRUE(line->duration_ns.has_value());
  EXPECT_EQ(*line->duration_ns, 4000);
}

TEST(StraceLineParserTest, ReturnValueColumnAlignedWithSpaces) {
  // strace right-aligns the '=' column across a trace with padding spaces
  // (so short calls like "close(2)" don't need to match the width of the
  // longest call name), so the gap between ')' and '=' is not always
  // exactly one space.
  auto line = ParseStraceLine(R"(1700000000.000000 close(2)        = 0)").line;
  ASSERT_TRUE(line.has_value());
  EXPECT_EQ(line->syscall, "close");
  EXPECT_EQ(line->args, "2");
  ASSERT_TRUE(line->return_value.has_value());
  EXPECT_EQ(*line->return_value, "0");
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

TEST(StraceLineParserTest, IsStraceFormatTraceSkipsLeadingDiagnostics) {
  // `strace -f -p <pid>` opens with an "attached" diagnostic rather than a
  // syscall line; the trace is still strace output.
  std::string trace =
      "strace: Process 1234 attached\n"
      "[pid  1234] 1700000000.000000 read(3, \"abc\", 1024) = 3 <0.000012>\n";
  EXPECT_TRUE(IsStraceFormatTrace(
      reinterpret_cast<const uint8_t*>(trace.data()), trace.size()));

  // Skipping diagnostics must not turn an unrelated format into strace.
  std::string not_strace =
      "strace: Process 1234 attached\n{\"traceEvents\": []}";
  EXPECT_FALSE(IsStraceFormatTrace(
      reinterpret_cast<const uint8_t*>(not_strace.data()), not_strace.size()));
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
