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

#include "perfetto/ext/base/regex.h"

#include <optional>
#include <string_view>
#include <vector>

#include "src/base/regex/regex_std.h"

#if PERFETTO_BUILDFLAG(PERFETTO_RE2)
#include "src/base/regex/regex_re2.h"
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_PCRE2)
#include "src/base/regex/regex_pcre2.h"
#endif

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

template <typename T>
struct BackendTraits;

template <>
struct BackendTraits<RegexStd> {
  static const char* name() { return "RegexStd"; }
};

#if PERFETTO_BUILDFLAG(PERFETTO_RE2)
template <>
struct BackendTraits<RegexRe2> {
  static const char* name() { return "RegexRe2"; }
};
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_PCRE2)
template <>
struct BackendTraits<RegexPcre2> {
  static const char* name() { return "RegexPcre2"; }
};
#endif

template <typename Backend>
class RegexBackendTest : public ::testing::Test {
 protected:
  StatusOr<Backend> Create(std::string_view pattern,
                           bool case_insensitive = false) {
    return Backend::Create(pattern, case_insensitive);
  }
};

using BackendTypes = ::testing::Types<RegexStd
#if PERFETTO_BUILDFLAG(PERFETTO_RE2)
                                      ,
                                      RegexRe2
#endif
#if PERFETTO_BUILDFLAG(PERFETTO_PCRE2)
                                      ,
                                      RegexPcre2
#endif
                                      >;
TYPED_TEST_SUITE(RegexBackendTest, BackendTypes, /* trailing ',' for GCC*/);

TYPED_TEST(RegexBackendTest, PartialMatch) {
  auto re_or = this->Create("abc");
  ASSERT_TRUE(re_or.ok());
  auto& re = re_or.value();
  EXPECT_TRUE(re.PartialMatch("abc"));
  EXPECT_TRUE(re.PartialMatch("xabcy"));
  EXPECT_FALSE(re.PartialMatch("abd"));
}

TYPED_TEST(RegexBackendTest, FullMatch) {
  auto re_or = this->Create("abc");
  ASSERT_TRUE(re_or.ok());
  auto& re = re_or.value();
  EXPECT_TRUE(re.FullMatch("abc"));
  EXPECT_FALSE(re.FullMatch("xabcy"));
  EXPECT_FALSE(re.FullMatch("abcd"));
}

TYPED_TEST(RegexBackendTest, PartialMatchWithGroups) {
  auto re_or = this->Create("([a-z]+)-([0-9]+)");
  ASSERT_TRUE(re_or.ok());
  auto& re = re_or.value();
  std::vector<std::string_view> matches;
  EXPECT_TRUE(re.PartialMatchWithGroups("foo-123", matches));
  ASSERT_EQ(matches.size(), 3u);
  EXPECT_EQ(matches[0], "foo-123");
  EXPECT_EQ(matches[1], "foo");
  EXPECT_EQ(matches[2], "123");
}

TYPED_TEST(RegexBackendTest, PartialMatchWithGroupsNoMatch) {
  auto re_or = this->Create("([a-z]+)-([0-9]+)");
  ASSERT_TRUE(re_or.ok());
  std::vector<std::string_view> matches;
  EXPECT_FALSE(re_or->PartialMatchWithGroups("foo-bar", matches));
  EXPECT_TRUE(matches.empty());
}

TYPED_TEST(RegexBackendTest, PartialMatchWithGroupsOptionalGroup) {
  auto re_or = this->Create("a(b)?c");
  ASSERT_TRUE(re_or.ok());
  std::vector<std::string_view> matches;
  EXPECT_TRUE(re_or->PartialMatchWithGroups("ac", matches));
  ASSERT_EQ(matches.size(), 2u);
  EXPECT_EQ(matches[0], "ac");
  EXPECT_TRUE(matches[1].empty());
}

TYPED_TEST(RegexBackendTest, FullMatchWithGroups) {
  auto re_or = this->Create("([a-z]+)-([0-9]+)");
  ASSERT_TRUE(re_or.ok());
  std::vector<std::string_view> matches;
  EXPECT_TRUE(re_or->FullMatchWithGroups("foo-123", matches));
  ASSERT_EQ(matches.size(), 3u);
  EXPECT_EQ(matches[0], "foo-123");
  EXPECT_EQ(matches[1], "foo");
  EXPECT_EQ(matches[2], "123");
  EXPECT_FALSE(re_or->FullMatchWithGroups("xfoo-123x", matches));
  EXPECT_TRUE(matches.empty());
}

TYPED_TEST(RegexBackendTest, GlobalReplace) {
  auto re_or = this->Create("a");
  ASSERT_TRUE(re_or.ok());
  EXPECT_EQ(re_or->GlobalReplace("aba", "x"), "xbx");
  EXPECT_EQ(re_or->GlobalReplace("bbb", "x"), "bbb");
  EXPECT_EQ(re_or->GlobalReplace("aba", ""), "b");
}

TYPED_TEST(RegexBackendTest, PerlShorthands) {
  {
    auto re_or = this->Create(R"(\d+)");
    ASSERT_TRUE(re_or.ok());
    EXPECT_TRUE(re_or->PartialMatch("abc123"));
    EXPECT_FALSE(re_or->PartialMatch("abcxyz"));
    EXPECT_TRUE(re_or->FullMatch("42"));
  }
  {
    auto re_or = this->Create(R"(\w+)");
    ASSERT_TRUE(re_or.ok());
    EXPECT_TRUE(re_or->FullMatch("hello_world123"));
    EXPECT_FALSE(re_or->FullMatch("hello world"));
  }
  {
    auto re_or = this->Create(R"(\s+)");
    ASSERT_TRUE(re_or.ok());
    EXPECT_TRUE(re_or->PartialMatch("hello world"));
    EXPECT_FALSE(re_or->PartialMatch("helloworld"));
  }
}

TYPED_TEST(RegexBackendTest, CaseInsensitive) {
  auto re_or = this->Create("abc", /*case_insensitive=*/true);
  ASSERT_TRUE(re_or.ok());
  EXPECT_TRUE(re_or->PartialMatch("ABC"));
  EXPECT_TRUE(re_or->PartialMatch("aBc"));
  EXPECT_TRUE(re_or->FullMatch("ABC"));
  EXPECT_FALSE(re_or->PartialMatch("abd"));
}

TYPED_TEST(RegexBackendTest, CaretMatchesOnlyStringStart) {
  auto re_or = this->Create("^abc");
  ASSERT_TRUE(re_or.ok());
  EXPECT_TRUE(re_or->PartialMatch("abc"));
  EXPECT_TRUE(re_or->PartialMatch("abcdef"));
  // ^ should NOT match after a newline (no MULTILINE).
  EXPECT_FALSE(re_or->PartialMatch("xyz\nabc"));
}

TYPED_TEST(RegexBackendTest, SearchWithOffset) {
  auto re_or = this->Create("([0-9]+)");
  ASSERT_TRUE(re_or.ok());
  std::vector<std::string_view> matches;
  std::string_view input = "abc123def456";
  // From offset 0 — should find "123".
  EXPECT_TRUE(re_or->SearchWithOffset(input, 0, matches));
  ASSERT_GE(matches.size(), 1u);
  EXPECT_EQ(matches[0], "123");
  // From offset 6 (past "123") — should find "456".
  EXPECT_TRUE(re_or->SearchWithOffset(input, 6, matches));
  ASSERT_GE(matches.size(), 1u);
  EXPECT_EQ(matches[0], "456");
  // From offset 12 (end) — no match.
  EXPECT_FALSE(re_or->SearchWithOffset(input, 12, matches));
}

// ============================================================================
// Tests for the Regex wrapper class (uses the active backend via RegexImpl).
// ============================================================================

TEST(RegexTest, PartialMatch) {
  auto re = Regex::CreateOrCheck("abc");
  EXPECT_TRUE(re.PartialMatch("abc"));
  EXPECT_TRUE(re.PartialMatch("xabcy"));
  EXPECT_FALSE(re.PartialMatch("abd"));
}

TEST(RegexTest, FullMatch) {
  auto re = Regex::CreateOrCheck("abc");
  EXPECT_TRUE(re.FullMatch("abc"));
  EXPECT_FALSE(re.FullMatch("xabcy"));
}

TEST(RegexTest, Move) {
  Regex re1 = Regex::CreateOrCheck("abc");
  EXPECT_TRUE(re1.PartialMatch("abc"));
  Regex re2 = std::move(re1);
  EXPECT_TRUE(re2.PartialMatch("abc"));
}

TEST(RegexTest, MoveAssignment) {
  Regex re1 = Regex::CreateOrCheck("abc");
  Regex re2 = Regex::CreateOrCheck("xyz");
  re2 = std::move(re1);
  EXPECT_TRUE(re2.PartialMatch("abc"));
  EXPECT_FALSE(re2.PartialMatch("xyz"));
}

TEST(RegexTest, EmptyString) {
  auto re = Regex::CreateOrCheck("abc");
  EXPECT_FALSE(re.PartialMatch(""));
  EXPECT_FALSE(re.FullMatch(""));
  std::vector<std::string_view> matches;
  EXPECT_FALSE(re.PartialMatchWithGroups("", matches));
}

TEST(RegexTest, Alternation) {
  auto re = Regex::CreateOrCheck("cat|dog");
  EXPECT_TRUE(re.PartialMatch("I have a cat"));
  EXPECT_TRUE(re.PartialMatch("I have a dog"));
  EXPECT_FALSE(re.PartialMatch("I have a bird"));
}

TEST(RegexTest, PartialMatchAll) {
  auto re = Regex::CreateOrCheck("([0-9]+)");
  auto it = re.PartialMatchAll("foo123bar456baz789");
  std::vector<std::string_view> groups;

  auto m1 = it.NextWithGroups(groups);
  ASSERT_TRUE(m1.has_value());
  EXPECT_EQ(*m1, "123");
  ASSERT_EQ(groups.size(), 2u);
  EXPECT_EQ(groups[1], "123");

  auto m2 = it.Next();
  ASSERT_TRUE(m2.has_value());
  EXPECT_EQ(*m2, "456");

  auto m3 = it.Next();
  ASSERT_TRUE(m3.has_value());
  EXPECT_EQ(*m3, "789");

  EXPECT_FALSE(it.Next().has_value());
}

TEST(RegexTest, PartialMatchAllNoMatch) {
  auto re = Regex::CreateOrCheck("[0-9]+");
  auto it = re.PartialMatchAll("hello");
  EXPECT_FALSE(it.Next().has_value());
}

// --- CopyableRegex tests ---

TEST(CopyableRegexTest, CopyConstruct) {
  CopyableRegex re1(Regex::CreateOrCheck("abc"));
  CopyableRegex re2(re1);
  EXPECT_TRUE(re2.PartialMatch("abc"));
  EXPECT_FALSE(re2.PartialMatch("xyz"));
  EXPECT_TRUE(re1.PartialMatch("abc"));
}

TEST(CopyableRegexTest, CopyAssignment) {
  CopyableRegex re1(Regex::CreateOrCheck("abc"));
  CopyableRegex re2(Regex::CreateOrCheck("xyz"));
  re2 = re1;
  EXPECT_TRUE(re2.PartialMatch("abc"));
  EXPECT_FALSE(re2.PartialMatch("xyz"));
  EXPECT_TRUE(re1.PartialMatch("abc"));
}

TEST(CopyableRegexTest, CaseInsensitiveCopy) {
  CopyableRegex re1(
      Regex::CreateOrCheck("abc", Regex::CaseSensitivity::kInsensitive));
  CopyableRegex re2(re1);
  EXPECT_TRUE(re2.PartialMatch("ABC"));
  EXPECT_FALSE(re2.PartialMatch("abd"));
}

TEST(CopyableRegexTest, MoveConstruct) {
  CopyableRegex re1(Regex::CreateOrCheck("abc"));
  CopyableRegex re2(std::move(re1));
  EXPECT_TRUE(re2.PartialMatch("abc"));
}

TEST(CopyableRegexTest, FullMatchWithGroups) {
  CopyableRegex re(Regex::CreateOrCheck("([a-z]+)-([0-9]+)"));
  std::vector<std::string_view> matches;
  EXPECT_TRUE(re.FullMatchWithGroups("foo-123", matches));
  ASSERT_EQ(matches.size(), 3u);
  EXPECT_EQ(matches[1], "foo");
  EXPECT_EQ(matches[2], "123");
}

}  // namespace
}  // namespace base
}  // namespace perfetto
