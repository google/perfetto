/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "test/gtest_and_gmock.h"

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "src/trace_processor/util/symbolizer/breakpad_parser.h"

namespace perfetto {
namespace profiling {

namespace {

// Used to initialize parser objects.
constexpr char kFakeFilePath[] = "bad/file/path";

TEST(BreakpadParserTest, FileIsEmpty) {
  base::TempFile file = base::TempFile::Create();
  BreakpadParser parser(file.path());
  ASSERT_TRUE(parser.ParseFile());
  EXPECT_TRUE(parser.symbols_for_testing().empty());
}

TEST(BreakpadParserTest, FileNotOpened) {
  BreakpadParser parser(kFakeFilePath);
  ASSERT_FALSE(parser.ParseFile());
  EXPECT_TRUE(parser.symbols_for_testing().empty());
}

TEST(BreakpadParserTest, ContainsNoFuncRecord) {
  BreakpadParser parser(kFakeFilePath);
  constexpr char kTestFileContents[] =
      "MODULE mac x86_64 E3A0F28FBCB43C15986D8608AF1DD2380 exif.so\n"
      "FILE 0 /Applications/../MacOSX10.10.sdk/usr/include/ctype.h\n"
      "1031 2 39 4\n"
      "STACK CFI 1014 .cfa: $rbp 16 +\n";
  ASSERT_TRUE(parser.ParseFromString(kTestFileContents));
  EXPECT_TRUE(parser.symbols_for_testing().empty());
  EXPECT_TRUE(parser.public_symbols_for_testing().empty());
}

TEST(BreakpadParserTest, ContainsOneFuncRecord) {
  BreakpadParser parser(kFakeFilePath);
  constexpr char kTestFileContents[] =
      "MODULE mac x86_64 E3A0F28FBCB43C15986D8608AF1DD2380 exif.so\n"
      "FUNC 1010 23 0 foo::bar()\n"
      "1031 2 39 4\n"
      "PUBLIC 2e7c0 0 items\n";
  ASSERT_TRUE(parser.ParseFromString(kTestFileContents));
  ASSERT_EQ(parser.symbols_for_testing().size(), 1u);
  ASSERT_EQ(parser.public_symbols_for_testing().size(), 1u);
  EXPECT_STREQ(parser.symbols_for_testing()[0].symbol_name.c_str(),
               "foo::bar()");
  EXPECT_EQ(parser.symbols_for_testing()[0].start_address,
            static_cast<uint64_t>(0x1010));
}

TEST(BreakpadParserTest, ContainsManyFuncRecords) {
  BreakpadParser parser(kFakeFilePath);
  constexpr char kTestFileContents[] =
      "MODULE mac x86_64 E3A0F28FBCB43C15986D8608AF1DD2380 exif.so\n"
      "FUNC 1010 23 0 foo_foo\n"
      "1031 2 39 4\n"
      "FUNC 1040 84 0 bar_1\n"
      "1040 4 44 5\n"
      "FUNC 10d0 6b 0 baz_baz()\n";
  ASSERT_TRUE(parser.ParseFromString(kTestFileContents));
  ASSERT_EQ(parser.symbols_for_testing().size(), 3u);
  EXPECT_STREQ(parser.symbols_for_testing()[0].symbol_name.c_str(), "foo_foo");
  EXPECT_EQ(parser.symbols_for_testing()[0].start_address,
            static_cast<uint64_t>(0x1010));
  EXPECT_EQ(parser.symbols_for_testing()[0].function_size, 35u);
  EXPECT_STREQ(parser.symbols_for_testing()[1].symbol_name.c_str(), "bar_1");
  EXPECT_EQ(parser.symbols_for_testing()[1].start_address,
            static_cast<uint64_t>(0x1040));
  EXPECT_EQ(parser.symbols_for_testing()[1].function_size, 132u);
  EXPECT_STREQ(parser.symbols_for_testing()[2].symbol_name.c_str(),
               "baz_baz()");
  EXPECT_EQ(parser.symbols_for_testing()[2].start_address,
            static_cast<uint64_t>(0x10d0));
  EXPECT_EQ(parser.symbols_for_testing()[2].function_size, 107u);
}

TEST(BreakpadParserTest, OptionalArgument) {
  BreakpadParser parser(kFakeFilePath);
  constexpr char kTestFileContents[] =
      "MODULE mac x86_64 E3A0F28FBCB43C15986D8608AF1DD2380 exif.so\n"
      "FUNC m 1010 23 0 foo_foo()\n"
      "1031 2 39 4\n"
      "FUNC m 1040 84 0 bar_1\n";
  ASSERT_TRUE(parser.ParseFromString(kTestFileContents));
  ASSERT_EQ(parser.symbols_for_testing().size(), 2u);
  EXPECT_STREQ(parser.symbols_for_testing()[0].symbol_name.c_str(),
               "foo_foo()");
  EXPECT_EQ(parser.symbols_for_testing()[0].start_address,
            static_cast<uint64_t>(0x1010));
  EXPECT_STREQ(parser.symbols_for_testing()[1].symbol_name.c_str(), "bar_1");
  EXPECT_EQ(parser.symbols_for_testing()[1].start_address,
            static_cast<uint64_t>(0x1040));
}

TEST(BreakpadParserTest, FuncNameWithSpaces) {
  BreakpadParser parser(kFakeFilePath);
  constexpr char kTestFileContents[] =
      "MODULE mac x86_64 E3A0F28FBCB43C15986D8608AF1DD2380 exif.so\n"
      "FUNC 1010 23 0 foo foo foo\n"
      "1031 2 39 4\n"
      "FUNC 1040 84 0 bar\n"
      "1040 4 44 5\n"
      "FUNC 10d0 6b 0 baz\n";
  ASSERT_TRUE(parser.ParseFromString(kTestFileContents));
  ASSERT_EQ(parser.symbols_for_testing().size(), 3u);
  EXPECT_STREQ(parser.symbols_for_testing()[0].symbol_name.c_str(),
               "foo foo foo");
  EXPECT_EQ(parser.symbols_for_testing()[0].start_address,
            static_cast<uint64_t>(0x1010));
  EXPECT_STREQ(parser.symbols_for_testing()[2].symbol_name.c_str(), "baz");
  EXPECT_EQ(parser.symbols_for_testing()[2].start_address,
            static_cast<uint64_t>(0x10d0));
}

TEST(BreakpadParserTest, NonHexAddress) {
  BreakpadParser parser(kFakeFilePath);
  constexpr char kTestFileContents[] =
      "MODULE mac x86_64 E3A0F28FBCB43C15986D8608AF1DD2380 exif.so\n"
      "FUNC foo 23 0 foo\n"
      "1031 2 39 4\n"
      "FUNC 1040 84 0 bar\n"
      "1040 4 44 5\n"
      "FUNC 10d0 6b 0 baz\n";
  ASSERT_FALSE(parser.ParseFromString(kTestFileContents));
  EXPECT_TRUE(parser.symbols_for_testing().empty());
}

TEST(BreakpadParserTest, NoModuleRecord) {
  BreakpadParser parser(kFakeFilePath);
  constexpr char kTestFileContents[] =
      "FUNC foo 23 0 foo()\n"
      "1031 2 39 4\n"
      "FUNC 1040 84 0 bar\n"
      "1040 4 44 5\n"
      "FUNC 10d0 6b 0 baz\n";
  ASSERT_FALSE(parser.ParseFromString(kTestFileContents));
  EXPECT_TRUE(parser.symbols_for_testing().empty());
}

// To make it easy to read, each FUNC record is followed by two LINE records:
// one showing the start address of the ending instruction and one showing the
// address where the function ends.
constexpr char kGetSymbolTestContents[] =
    "MODULE mac x86_64 E3A0F28FBCB43C15986D8608AF1DD2380 exif.so\n"
    "FUNC 1010 23 0 foo\n"
    "1031 2 39 4\n"
    "1033 0 0 0\n"
    "FUNC 1040 84 0 bar\n"
    "10b6 e 44 5\n"
    "10c4 0 0 0\n"
    "FUNC 10d0 6b 0 baz\n"
    "1136 5 44 5\n"
    "113b 0 0 0\n"
    "PUBLIC 12010 0 p_foo\n"
    "PUBLIC 12018 0 p_bar\n"
    "PUBLIC 12050 0 p_bax\n"
    "PUBLIC 12090 0 p_baz\n";

TEST(BreakpadParserTest, GivenStartAddr) {
  BreakpadParser parser(kFakeFilePath);
  ASSERT_TRUE(parser.ParseFromString(kGetSymbolTestContents));
  ASSERT_EQ(parser.symbols_for_testing().size(), 3u);
  EXPECT_EQ(*parser.GetSymbol(0x1010U), "foo");
  EXPECT_EQ(*parser.GetSymbol(0x10d0U), "baz");

  ASSERT_EQ(parser.public_symbols_for_testing().size(), 4u);
  EXPECT_EQ(*parser.GetPublicSymbol(0x12010U), "p_foo");
  EXPECT_EQ(*parser.GetPublicSymbol(0x12018U), "p_bar");
  EXPECT_EQ(*parser.GetPublicSymbol(0x12050U), "p_bax");
  EXPECT_FALSE(parser.GetPublicSymbol(0x12090U));
}

TEST(BreakpadParserTest, GivenAddrInRange) {
  BreakpadParser parser(kFakeFilePath);
  ASSERT_TRUE(parser.ParseFromString(kGetSymbolTestContents));
  ASSERT_EQ(parser.symbols_for_testing().size(), 3u);
  EXPECT_EQ(*parser.GetSymbol(0x1030U), "foo");
  EXPECT_EQ(*parser.GetSymbol(0x10c0U), "bar");

  ASSERT_EQ(parser.public_symbols_for_testing().size(), 4u);
  EXPECT_EQ(*parser.GetPublicSymbol(0x12014U), "p_foo");
  EXPECT_EQ(*parser.GetPublicSymbol(0x12038U), "p_bar");
  EXPECT_EQ(*parser.GetPublicSymbol(0x12068U), "p_bax");
}

TEST(BreakpadParserTest, AddrTooLow) {
  BreakpadParser parser(kFakeFilePath);
  ASSERT_TRUE(parser.ParseFromString(kGetSymbolTestContents));
  ASSERT_EQ(parser.symbols_for_testing().size(), 3u);
  EXPECT_FALSE(parser.GetSymbol(0x1000U));

  ASSERT_EQ(parser.public_symbols_for_testing().size(), 4u);
  EXPECT_FALSE(parser.GetPublicSymbol(0x12000U));
}

TEST(BreakpadParserTest, AddrTooHigh) {
  BreakpadParser parser(kFakeFilePath);
  ASSERT_TRUE(parser.ParseFromString(kGetSymbolTestContents));
  ASSERT_EQ(parser.symbols_for_testing().size(), 3u);
  EXPECT_FALSE(parser.GetSymbol(0x3000U));

  ASSERT_EQ(parser.public_symbols_for_testing().size(), 4u);
  EXPECT_FALSE(parser.GetPublicSymbol(0x15000U));
}

TEST(BreakpadParserTest, AddrBetweenFunctions) {
  BreakpadParser parser(kFakeFilePath);
  ASSERT_TRUE(parser.ParseFromString(kGetSymbolTestContents));
  ASSERT_EQ(parser.symbols_for_testing().size(), 3u);
  EXPECT_FALSE(parser.GetSymbol(0x1036U));
}

// Test file contents for GetSourceLocation tests. Contains FILE and LINE
// records that map machine code addresses to source file locations.
constexpr char kGetSourceLocationTestContents[] =
    "MODULE mac x86_64 E3A0F28FBCB43C15986D8608AF1DD2380 exif.so\n"
    "FILE 0 /path/to/foo.cc\n"
    "FILE 1 /path/to/bar.cc\n"
    "FILE 2 /path/to/baz.cc\n"
    "FUNC 1010 23 0 foo\n"
    "1010 10 10 0\n"
    "1020 13 20 0\n"
    "FUNC 1040 84 0 bar\n"
    "1040 40 100 1\n"
    "1080 44 150 1\n"
    "FUNC 10d0 6b 0 baz\n"
    "10d0 30 200 2\n"
    "1100 3b 250 2\n";

TEST(BreakpadParserTest, ContainsFileRecords) {
  BreakpadParser parser(kFakeFilePath);
  ASSERT_TRUE(parser.ParseFromString(kGetSourceLocationTestContents));
  // Verify that FILE records are parsed by checking GetSourceLocation returns
  // the correct file names.
  auto result = parser.GetSourceLocation(0x1010U);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(std::get<0>(*result), "/path/to/foo.cc");
}

TEST(BreakpadParserTest, ContainsLineRecords) {
  BreakpadParser parser(kFakeFilePath);
  ASSERT_TRUE(parser.ParseFromString(kGetSourceLocationTestContents));
  // Verify that LINE records are parsed by checking GetSourceLocation returns
  // the correct line numbers.
  auto result = parser.GetSourceLocation(0x1010U);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(std::get<1>(*result), 10u);
}

TEST(BreakpadParserTest, GetSourceLocationStartAddr) {
  BreakpadParser parser(kFakeFilePath);
  ASSERT_TRUE(parser.ParseFromString(kGetSourceLocationTestContents));
  // Test with exact start addresses.
  auto result1 = parser.GetSourceLocation(0x1010U);
  ASSERT_TRUE(result1.has_value());
  EXPECT_EQ(std::get<0>(*result1), "/path/to/foo.cc");
  EXPECT_EQ(std::get<1>(*result1), 10u);

  auto result2 = parser.GetSourceLocation(0x1040U);
  ASSERT_TRUE(result2.has_value());
  EXPECT_EQ(std::get<0>(*result2), "/path/to/bar.cc");
  EXPECT_EQ(std::get<1>(*result2), 100u);

  auto result3 = parser.GetSourceLocation(0x10d0U);
  ASSERT_TRUE(result3.has_value());
  EXPECT_EQ(std::get<0>(*result3), "/path/to/baz.cc");
  EXPECT_EQ(std::get<1>(*result3), 200u);
}

TEST(BreakpadParserTest, GetSourceLocationInRange) {
  BreakpadParser parser(kFakeFilePath);
  ASSERT_TRUE(parser.ParseFromString(kGetSourceLocationTestContents));
  // Test with addresses within the range of a line record.
  auto result = parser.GetSourceLocation(0x1015U);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(std::get<0>(*result), "/path/to/foo.cc");
  EXPECT_EQ(std::get<1>(*result), 10u);
}

TEST(BreakpadParserTest, GetSourceLocationAddrTooLow) {
  BreakpadParser parser(kFakeFilePath);
  ASSERT_TRUE(parser.ParseFromString(kGetSourceLocationTestContents));
  // Address is lower than any line record.
  EXPECT_FALSE(parser.GetSourceLocation(0x1000U).has_value());
}

TEST(BreakpadParserTest, GetSourceLocationAddrTooHigh) {
  BreakpadParser parser(kFakeFilePath);
  ASSERT_TRUE(parser.ParseFromString(kGetSourceLocationTestContents));
  // Address is higher than any line record.
  EXPECT_FALSE(parser.GetSourceLocation(0x3000U).has_value());
}

TEST(BreakpadParserTest, GetSourceLocationAddrBetweenRecords) {
  BreakpadParser parser(kFakeFilePath);
  ASSERT_TRUE(parser.ParseFromString(kGetSourceLocationTestContents));
  // Address falls between line records (in the gap between functions).
  EXPECT_FALSE(parser.GetSourceLocation(0x1035U).has_value());
}

TEST(BreakpadParserTest, FileRecordIncomplete) {
  BreakpadParser parser(kFakeFilePath);
  constexpr char kTestFileContents[] =
      "MODULE mac x86_64 E3A0F28FBCB43C15986D8608AF1DD2380 exif.so\n"
      "FILE 0\n";
  ASSERT_FALSE(parser.ParseFromString(kTestFileContents));
}

TEST(BreakpadParserTest, FileRecordInvalidNumber) {
  BreakpadParser parser(kFakeFilePath);
  constexpr char kTestFileContents[] =
      "MODULE mac x86_64 E3A0F28FBCB43C15986D8608AF1DD2380 exif.so\n"
      "FILE abc /path/to/file.cc\n";
  ASSERT_FALSE(parser.ParseFromString(kTestFileContents));
}

TEST(BreakpadParserTest, LineRecordIncomplete) {
  BreakpadParser parser(kFakeFilePath);
  constexpr char kTestFileContents[] =
      "MODULE mac x86_64 E3A0F28FBCB43C15986D8608AF1DD2380 exif.so\n"
      "1010 10 20\n";
  ASSERT_FALSE(parser.ParseFromString(kTestFileContents));
}

TEST(BreakpadParserTest, LineRecordInvalidAddress) {
  BreakpadParser parser(kFakeFilePath);
  constexpr char kTestFileContents[] =
      "MODULE mac x86_64 E3A0F28FBCB43C15986D8608AF1DD2380 exif.so\n"
      "gggg 10 20 0\n";
  ASSERT_FALSE(parser.ParseFromString(kTestFileContents));
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
