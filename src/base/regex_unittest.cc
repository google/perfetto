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

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

TEST(RegexTest, Search) {
  auto re_or = Regex::Create("abc");
  ASSERT_TRUE(re_or.ok());
  auto& re = *re_or;

  EXPECT_TRUE(re.Search("abc"));
  EXPECT_TRUE(re.Search("xabcy"));
  EXPECT_TRUE(re.Search("abcabc"));
  EXPECT_FALSE(re.Search("abx"));
}

TEST(RegexTest, SearchPartial) {
  auto re_or = Regex::Create("a.*c");
  ASSERT_TRUE(re_or.ok());
  auto& re = *re_or;

  EXPECT_TRUE(re.Search("abc"));
  EXPECT_TRUE(re.Search("abbbc"));
  EXPECT_TRUE(re.Search("ac"));
}

TEST(RegexTest, Invalid) {
  auto re_or = Regex::Create("[a-z");
  EXPECT_FALSE(re_or.ok());
}

TEST(RegexTest, Move) {
  auto re_or = Regex::Create("abc");
  ASSERT_TRUE(re_or.ok());
  Regex re1 = std::move(*re_or);
  Regex re2 = std::move(re1);
  EXPECT_TRUE(re2.Search("abc"));
}

TEST(RegexTest, Submatch) {
  auto re_or = Regex::Create("a(b)c(d)e");
  ASSERT_TRUE(re_or.ok());
  auto& re = *re_or;

  std::vector<std::string_view> matches;
  re.Submatch("abcde", matches);
  ASSERT_FALSE(matches.empty());
  EXPECT_THAT(matches, testing::ElementsAre("abcde", "b", "d"));
}

TEST(RegexTest, SubmatchNoMatch) {
  auto re_or = Regex::Create("a(b)c(d)e");
  ASSERT_TRUE(re_or.ok());
  auto& re = *re_or;

  std::vector<std::string_view> matches;
  re.Submatch("fghij", matches);
  ASSERT_TRUE(matches.empty());
}

TEST(RegexTest, SubmatchOptionalGroup) {
  auto re_or = Regex::Create("a(b)?c");
  ASSERT_TRUE(re_or.ok());
  auto& re = *re_or;

  std::vector<std::string_view> matches;
  re.Submatch("ac", matches);
  ASSERT_FALSE(matches.empty());
  EXPECT_THAT(matches, testing::ElementsAre("ac", ""));
}

}  // namespace
}  // namespace base
}  // namespace perfetto
