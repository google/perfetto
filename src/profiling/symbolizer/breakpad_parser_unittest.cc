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
#include "src/profiling/symbolizer/breakpad_parser.h"

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
      "PUBLIC 313c0 0 items\n"
      "STACK CFI 1014 .cfa: $rbp 16 +\n";
  ASSERT_TRUE(parser.ParseFromString(kTestFileContents));
  EXPECT_TRUE(parser.symbols_for_testing().empty());
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
    "113b 0 0 0\n";

TEST(BreakpadParserTest, GivenStartAddr) {
  BreakpadParser parser(kFakeFilePath);
  ASSERT_TRUE(parser.ParseFromString(kGetSymbolTestContents));
  ASSERT_EQ(parser.symbols_for_testing().size(), 3u);
  EXPECT_EQ(*parser.GetSymbol(0x1010U), "foo");
  EXPECT_EQ(*parser.GetSymbol(0x10d0U), "baz");
}

TEST(BreakpadParserTest, GivenAddrInRange) {
  BreakpadParser parser(kFakeFilePath);
  ASSERT_TRUE(parser.ParseFromString(kGetSymbolTestContents));
  ASSERT_EQ(parser.symbols_for_testing().size(), 3u);
  EXPECT_EQ(*parser.GetSymbol(0x1030U), "foo");
  EXPECT_EQ(*parser.GetSymbol(0x10c0U), "bar");
}

TEST(BreakpadParserTest, AddrTooLow) {
  BreakpadParser parser(kFakeFilePath);
  ASSERT_TRUE(parser.ParseFromString(kGetSymbolTestContents));
  ASSERT_EQ(parser.symbols_for_testing().size(), 3u);
  EXPECT_FALSE(parser.GetSymbol(0x1000U));
}

TEST(BreakpadParserTest, AddrTooHigh) {
  BreakpadParser parser(kFakeFilePath);
  ASSERT_TRUE(parser.ParseFromString(kGetSymbolTestContents));
  ASSERT_EQ(parser.symbols_for_testing().size(), 3u);
  EXPECT_FALSE(parser.GetSymbol(0x3000U));
}

TEST(BreakpadParserTest, AddrBetweenFunctions) {
  BreakpadParser parser(kFakeFilePath);
  ASSERT_TRUE(parser.ParseFromString(kGetSymbolTestContents));
  ASSERT_EQ(parser.symbols_for_testing().size(), 3u);
  EXPECT_FALSE(parser.GetSymbol(0x1036U));
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
