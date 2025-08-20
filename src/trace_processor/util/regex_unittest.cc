/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "src/trace_processor/util/regex.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::regex {
namespace {

TEST(Regex, Submatch) {
  auto regex = std::move(*Regex::Create("a(b)c(d)e"));
  std::vector<std::string_view> matches;
  regex.Submatch("abcde", matches);
  ASSERT_FALSE(matches.empty());
  EXPECT_THAT(matches, testing::ElementsAre("abcde", "b", "d"));
}

TEST(Regex, SubmatchNoMatch) {
  auto regex = std::move(*Regex::Create("a(b)c(d)e"));
  std::vector<std::string_view> matches;
  regex.Submatch("fghij", matches);
  ASSERT_TRUE(matches.empty());
}

TEST(Regex, SubmatchOptionalGroup) {
  auto regex = std::move(*Regex::Create("a(b)?c"));
  std::vector<std::string_view> matches;
  regex.Submatch("ac", matches);
  ASSERT_FALSE(matches.empty());
  EXPECT_THAT(matches, testing::ElementsAre("ac", ""));
}

}  // namespace
}  // namespace perfetto::trace_processor::regex
