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

#include "perfetto/ext/base/regex.h"

#include <string_view>
#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

TEST(RegexTest, Search) {
  auto re_or = Regex::Create("abc");
  ASSERT_TRUE(re_or.ok());
  auto& re = re_or.value();
  EXPECT_TRUE(re.Search("abc"));
  EXPECT_TRUE(re.Search("xabcy"));
  EXPECT_FALSE(re.Search("abd"));
}

TEST(RegexTest, FullMatch) {
  auto re_or = Regex::Create("abc");
  ASSERT_TRUE(re_or.ok());
  auto& re = re_or.value();
  EXPECT_TRUE(re.FullMatch("abc"));
  EXPECT_FALSE(re.FullMatch("xabcy"));
  EXPECT_FALSE(re.FullMatch("abcd"));
}

TEST(RegexTest, SearchPartial) {
  auto re_or = Regex::Create("a.c");
  ASSERT_TRUE(re_or.ok());
  auto& re = re_or.value();
  EXPECT_TRUE(re.Search("abc"));
  EXPECT_TRUE(re.Search("axc"));
  EXPECT_FALSE(re.Search("abbc"));
}

TEST(RegexTest, Invalid) {
  auto re_or = Regex::Create("[");
  EXPECT_FALSE(re_or.ok());
}

TEST(RegexTest, Move) {
  auto re_or = Regex::Create("abc");
  Regex re1 = std::move(re_or.value());
  EXPECT_TRUE(re1.Search("abc"));
  Regex re2;
  re2 = std::move(re1);
  EXPECT_TRUE(re2.Search("abc"));
}

TEST(RegexTest, Submatch) {
  auto re_or = Regex::Create("([a-z]+)-([0-9]+)");
  ASSERT_TRUE(re_or.ok());
  auto& re = re_or.value();
  std::vector<std::string_view> matches;
  EXPECT_TRUE(re.Submatch("foo-123", matches));
  ASSERT_EQ(matches.size(), 3u);
  EXPECT_EQ(matches[0], "foo-123");
  EXPECT_EQ(matches[1], "foo");
  EXPECT_EQ(matches[2], "123");
}

TEST(RegexTest, SubmatchNoMatch) {
  auto re_or = Regex::Create("([a-z]+)-([0-9]+)");
  ASSERT_TRUE(re_or.ok());
  auto& re = re_or.value();
  std::vector<std::string_view> matches;
  EXPECT_FALSE(re.Submatch("foo-bar", matches));
  EXPECT_TRUE(matches.empty());
}

TEST(RegexTest, SubmatchOptionalGroup) {
  auto re_or = Regex::Create("a(b)?c");
  ASSERT_TRUE(re_or.ok());
  auto& re = re_or.value();
  std::vector<std::string_view> matches;
  EXPECT_TRUE(re.Submatch("ac", matches));
  ASSERT_EQ(matches.size(), 2u);
  EXPECT_EQ(matches[0], "ac");
  EXPECT_EQ(matches[1], "");
  EXPECT_TRUE(matches[1].data() == nullptr || matches[1].empty());
}

TEST(RegexTest, Replace) {
  auto re_or = Regex::Create("a");
  ASSERT_TRUE(re_or.ok());
  auto& re = re_or.value();
  EXPECT_EQ(re.Replace("aba", "x"), "xbx");
  EXPECT_EQ(re.Replace("bbb", "x"), "bbb");
}

}  // namespace
}  // namespace base
}  // namespace perfetto
