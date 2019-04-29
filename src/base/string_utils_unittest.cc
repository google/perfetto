/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "perfetto/base/string_utils.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace perfetto {
namespace base {
namespace {

using testing::ElementsAre;

TEST(StringUtilsTest, StartsWith) {
  EXPECT_TRUE(StartsWith("", ""));
  EXPECT_TRUE(StartsWith("abc", ""));
  EXPECT_TRUE(StartsWith("abc", "a"));
  EXPECT_TRUE(StartsWith("abc", "ab"));
  EXPECT_TRUE(StartsWith("abc", "abc"));
  EXPECT_FALSE(StartsWith("abc", "abcd"));
  EXPECT_FALSE(StartsWith("aa", "ab"));
  EXPECT_FALSE(StartsWith("", "ab"));
}

TEST(StringUtilsTest, EndsWith) {
  EXPECT_TRUE(EndsWith("", ""));
  EXPECT_TRUE(EndsWith("abc", ""));
  EXPECT_TRUE(EndsWith("abc", "c"));
  EXPECT_TRUE(EndsWith("abc", "bc"));
  EXPECT_TRUE(EndsWith("abc", "abc"));
  EXPECT_FALSE(EndsWith("bcd", "abcd"));
  EXPECT_FALSE(EndsWith("abc", "abd"));
  EXPECT_FALSE(EndsWith("", "c"));
}

TEST(StringUtilsTest, SplitString) {
  EXPECT_THAT(SplitString("", ":"), ElementsAre(""));
  EXPECT_THAT(SplitString("a:b:c", ":"), ElementsAre("a", "b", "c"));
  EXPECT_THAT(SplitString("a::b::c", "::"), ElementsAre("a", "b", "c"));
  EXPECT_THAT(SplitString("abc", ":"), ElementsAre("abc"));
  EXPECT_THAT(SplitString("abc", "::"), ElementsAre("abc"));
  EXPECT_THAT(SplitString("abc", ":"), ElementsAre("abc"));
  EXPECT_THAT(SplitString("abc", "::"), ElementsAre("abc"));
}

}  // namespace
}  // namespace base
}  // namespace perfetto
