/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_redaction/trace_redactor_options.h"

#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_redaction {
namespace {

TEST(TraceRedactorOptionsTest, AcceptsValidUids) {
  const std::vector<std::pair<std::string_view, uint64_t>> test_cases = {
      {"0", 0},
      {"1", 1},
      {"1000", 1000},
      {"10234", 10234},
      {"1010234", 1010234},
      {"2147483647", 2147483647},  // INT32_MAX
  };

  for (const auto& [str, expected] : test_cases) {
    TraceRedactorOptions options;
    EXPECT_TRUE(ParseUidOption(str, &options)) << "Failed for: " << str;
    ASSERT_TRUE(options.target_uid.has_value());
    EXPECT_EQ(*options.target_uid, expected);
  }
}

TEST(TraceRedactorOptionsTest, RejectsLeadingAndTrailingWhitespace) {
  const std::vector<std::string_view> invalid_cases = {
      " 10234", "10234 ", "\t10234", "\n10234", " 0", "0 ", " 10234 ",
  };

  for (const auto& str : invalid_cases) {
    TraceRedactorOptions options;
    EXPECT_FALSE(ParseUidOption(str, &options))
        << "Should reject whitespace: " << str;
    EXPECT_FALSE(options.target_uid.has_value());
  }
}

TEST(TraceRedactorOptionsTest, RejectsExplicitSigns) {
  const std::vector<std::string_view> invalid_cases = {
      "+10234", "-10234", " -10234", "+0", "-0",
  };

  for (const auto& str : invalid_cases) {
    TraceRedactorOptions options;
    EXPECT_FALSE(ParseUidOption(str, &options))
        << "Should reject sign: " << str;
    EXPECT_FALSE(options.target_uid.has_value());
  }
}

TEST(TraceRedactorOptionsTest, RejectsNonDigitCharacters) {
  const std::vector<std::string_view> invalid_cases = {
      "10a234", "10234.0", "0x10", "abc", "12-34", "12_34",
  };

  for (const auto& str : invalid_cases) {
    TraceRedactorOptions options;
    EXPECT_FALSE(ParseUidOption(str, &options))
        << "Should reject non-digits: " << str;
    EXPECT_FALSE(options.target_uid.has_value());
  }
}

TEST(TraceRedactorOptionsTest, RejectsEmptyString) {
  TraceRedactorOptions options;
  EXPECT_FALSE(ParseUidOption("", &options));
  EXPECT_FALSE(options.target_uid.has_value());
}

TEST(TraceRedactorOptionsTest, RejectsOutOfRange) {
  const std::vector<std::string_view> invalid_cases = {
      "2147483648",            // INT32_MAX + 1
      "4294967295",            // UINT32_MAX
      "18446744073709551615",  // UINT64_MAX
      "18446744073709551616",  // Overflow > UINT64_MAX
      "999999999999999999999",
  };

  for (const auto& str : invalid_cases) {
    TraceRedactorOptions options;
    EXPECT_FALSE(ParseUidOption(str, &options))
        << "Should reject out-of-range: " << str;
    EXPECT_FALSE(options.target_uid.has_value());
  }
}

TEST(TraceRedactorOptionsTest, RejectsDuplicateUidFlag) {
  char bin[] = "trace_redactor";
  char in[] = "in.pftrace";
  char out[] = "out.pftrace";
  char pkg[] = "com.example.app";
  char flag1[] = "--uid";
  char val1[] = "1000";
  char flag2[] = "--uid";
  char val2[] = "2000";
  char* argv[] = {bin, in, out, pkg, flag1, val1, flag2, val2};
  int argc = sizeof(argv) / sizeof(argv[0]);

  TraceRedactorOptions options;
  EXPECT_FALSE(ParseOptionalArgs(argc, argv, 4, &options));
}

TEST(TraceRedactorOptionsTest, RejectsMissingUidValue) {
  char bin[] = "trace_redactor";
  char in[] = "in.pftrace";
  char out[] = "out.pftrace";
  char pkg[] = "com.example.app";
  char flag[] = "--uid";
  char* argv[] = {bin, in, out, pkg, flag};
  int argc = sizeof(argv) / sizeof(argv[0]);

  TraceRedactorOptions options;
  EXPECT_FALSE(ParseOptionalArgs(argc, argv, 4, &options));
}

TEST(TraceRedactorOptionsTest, RejectsUnknownFlag) {
  char bin[] = "trace_redactor";
  char in[] = "in.pftrace";
  char out[] = "out.pftrace";
  char pkg[] = "com.example.app";
  char flag[] = "--user";
  char val[] = "10";
  char* argv[] = {bin, in, out, pkg, flag, val};
  int argc = sizeof(argv) / sizeof(argv[0]);

  TraceRedactorOptions options;
  EXPECT_FALSE(ParseOptionalArgs(argc, argv, 4, &options));
}

TEST(TraceRedactorOptionsTest, ParsesValidOptionalArgs) {
  char bin[] = "trace_redactor";
  char in[] = "in.pftrace";
  char out[] = "out.pftrace";
  char pkg[] = "com.example.app";
  char flag[] = "--uid";
  char val[] = "1010234";
  char* argv[] = {bin, in, out, pkg, flag, val};
  int argc = sizeof(argv) / sizeof(argv[0]);

  TraceRedactorOptions options;
  EXPECT_TRUE(ParseOptionalArgs(argc, argv, 4, &options));
  ASSERT_TRUE(options.target_uid.has_value());
  EXPECT_EQ(*options.target_uid, 1010234u);
}

}  // namespace
}  // namespace perfetto::trace_redaction
