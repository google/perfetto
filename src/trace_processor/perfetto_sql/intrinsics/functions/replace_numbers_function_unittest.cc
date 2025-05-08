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
#include "src/trace_processor/perfetto_sql/intrinsics/functions/replace_numbers_function.h"

#include <string>

#include "perfetto/base/logging.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace test {
namespace {

TEST(ReplaceNumbersFunctionTest, TestReplaceWithPrefix) {
  std::string result = SqlStripHex("0x1234", 3);
  ASSERT_STREQ(result.c_str(), "0x<num>");
}

TEST(ReplaceNumbersFunctionTest, TestReplaceNonDigitHexAfter0x) {
  std::string result = SqlStripHex("0xabcd", 3);
  ASSERT_STREQ(result.c_str(), "0x<num>");
}

TEST(ReplaceNumbersFunctionTest, TestReplaceAtTheStart) {
  std::string result = SqlStripHex("12a34", 3);
  ASSERT_STREQ(result.c_str(), "<num>");
}

TEST(ReplaceNumbersFunctionTest, TestReplaceAfterSpace) {
  std::string result = SqlStripHex("Hello 123", 3);
  ASSERT_STREQ(result.c_str(), "Hello <num>");
}

TEST(ReplaceNumbersFunctionTest, TestReplaceOnlyDigits) {
  std::string result = SqlStripHex("abc", 1);
  ASSERT_STREQ(result.c_str(), "abc");
  result = SqlStripHex("#1 ImageDecoder#decodeDrawable", 1);
  ASSERT_STREQ(result.c_str(), "#<num> ImageDecoder#decodeDrawable");
}

TEST(ReplaceNumbersFunctionTest, TestReplaceOnlyGreaterThanRepeated) {
  std::string result = SqlStripHex("1=22@333-444", 3);
  ASSERT_STREQ(result.c_str(), "1=22@<num>-<num>");
}

TEST(ReplaceNumbersFunctionTest, TestReplaceDoingNothing) {
  std::string result = SqlStripHex("aaaaaa", 1);
  ASSERT_STREQ(result.c_str(), "aaaaaa");
}

TEST(ReplaceNumbersFunctionTest,
     TestReplaceSpecialPrefixAfterNonAlphaNumericChar) {
  std::string result = SqlStripHex(
      "=0x1234 InputConsumer on 0x1234 Controller (0x75dfea9cc0)", 3);
  ASSERT_STREQ(result.c_str(),
               "=0x<num> InputConsumer on 0x<num> Controller (0x<num>)");
}

TEST(ReplaceNumbersFunctionTest, TestReplaceDigitsWithoutPrefix) {
  std::string result =
      SqlStripHex("connector: metadata20 response_metadata 100x100", 2);
  ASSERT_STREQ(result.c_str(),
               "connector: metadata<num> response_metadata <num>x<num>");
}

}  // namespace
}  // namespace test
}  // namespace trace_processor
}  // namespace perfetto
