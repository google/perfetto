/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "perfetto/ext/base/proc_utils.h"
#include "perfetto/base/build_config.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/thread_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::base {
namespace {

TEST(SplitProcStatStringTest, CorrectRealStrings) {
  constexpr char kFullString[] =
      "104315 (cat) R 16526 104315 16526 34818 104315 4194304 107 0 0 0 0 0 0 "
      "0 20 0 1 0 4716098 5754880 226 18446744073709551615 94372780531712 "
      "94372780551593 140724200813584 0 0 0 0 0 0 0 0 0 17 5 0 0 0 0 0 "
      "94372780567600 94372780569216 94372811526144 140724200820773 "
      "140724200820793 140724200820793 140724200841195 0";

  const auto res = SplitProcStatString(kFullString).value();
  ASSERT_EQ(res[0], "104315");
  ASSERT_EQ(res[1], "(cat)");
  ASSERT_EQ(res[50], "140724200841195");
  ASSERT_EQ(res[51], "0");
  ASSERT_EQ(res.size(), 52ul);
}

TEST(SplitProcStatStringTest, CorrectParensStrings) {
  // This is how /proc/self/stat can start for a binary called a\)\ \(b
  constexpr char kParensString[] = "123 (a) (b) R 5 6";
  const auto res = SplitProcStatString(kParensString).value();
  ASSERT_EQ(res[1], "(a) (b)");
  ASSERT_EQ(res[2], "R");
  ASSERT_EQ(res.size(), 5ul);
}

TEST(SplitProcStatStringTest, CorrectEmptyCommStrings) {
  constexpr char kEmptyCommString[] = "123 () R 5 6";
  const auto res = SplitProcStatString(kEmptyCommString).value();
  ASSERT_EQ(res[1], "()");
  ASSERT_EQ(res[2], "R");
  ASSERT_EQ(res.size(), 5ul);
}

TEST(SplitProcStatStringTest, EmptyString) {
  constexpr char kEmptyString[] = "";
  const auto res = SplitProcStatString(kEmptyString);
  ASSERT_FALSE(res.has_value());
}

TEST(SplitProcStatStringTest, TooShortString) {
  constexpr char kEndsAfterComm[] = "123 ()";
  const auto res = SplitProcStatString(kEndsAfterComm);
  ASSERT_FALSE(res.has_value());
}

TEST(SplitProcStatStringTest, NoPidString) {
  constexpr char kNoPid[] = "(cat) R 5 6";
  const auto res = SplitProcStatString(kNoPid);
  ASSERT_FALSE(res.has_value());
}

TEST(SplitProcStatStringTest, MissingParensStrings) {
  constexpr char kNoParens[] = "123 cat R 5 6";
  const auto res = SplitProcStatString(kNoParens);
  ASSERT_FALSE(res.has_value());
}

TEST(SplitProcStatStringTest, GarbageInGarbageOut) {
  // Test we don't crash on incorrect input.
  constexpr char kNoSpaceAfterPid[] = "12(3 cat) R 5 6";
  const auto res = SplitProcStatString(kNoSpaceAfterPid);
  ASSERT_TRUE(res.has_value());
  // We don't check the format of the string, so it will be successfully split.
  ASSERT_EQ(res.value().size(), 5ul);
  ASSERT_EQ(res.value()[0], "1");
  ASSERT_EQ(res.value()[1], "(3 cat)");
}

// ParseProcessStatTest tests are adapted from
// http://google3/base/sysinfo_unittest.cc;l=1521;rcl=747578803
TEST(ParseProcessStatTest, ReadSelfStat) {
  const auto res = ReadProcSelfStatFile();
  ASSERT_TRUE(res.has_value());
  const auto& parts = res.value();

  ASSERT_EQ(parts[0], std::to_string(getpid()));
}

class ScopedThreadName {
 public:
  explicit ScopedThreadName(const std::string& name) {
    GetThreadName(old_name_);
    MaybeSetThreadName(name);
  }
  ~ScopedThreadName() { MaybeSetThreadName(old_name_); }

 private:
  std::string old_name_;
};

TEST(ParseProcessStatTest, ParseSelfThreadNameWithSpaces) {
  ScopedThreadName scoped_thread_name(") )(ab");
  std::string stat;
  const StackString<256> tid_path("/proc/self/task/%d/stat", gettid());
  ASSERT_TRUE(ReadFile(tid_path.ToStdString(), &stat));
  const auto res = SplitProcStatString(stat);
  ASSERT_TRUE(res.has_value());
  const auto& parts = res.value();
  ASSERT_EQ(parts[1], "() )(ab)");
}

TEST(ParseProcessStatTest, StatState) {
  // Try to trick the parser into reading state Z by making the stat line start:
  // <pid> () Z ) ...
  ScopedThreadName scoped_thread_name(") Z");

  const auto res = ReadProcSelfStatFile();
  ASSERT_TRUE(res.has_value());
  const auto& parts = res.value();

  ASSERT_EQ(parts[1], "() Z)");
  ASSERT_EQ(parts[2], "R");
}

}  // namespace
}  // namespace perfetto::base

#endif
