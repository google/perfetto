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

#include <stdlib.h>

#include "test/gtest_and_gmock.h"

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/temp_file.h"
#include "src/profiling/symbolizer/breakpad_symbolizer.h"

namespace perfetto {
namespace profiling {

namespace {

TEST(BreakpadSymbolizerTest, NonExistantFile) {
  const std::string kBadFilePath = "/bad/file/path";
  constexpr char kTestDir[] = "Unused";
  BreakpadSymbolizer symbolizer(kTestDir);
  symbolizer.SetBreakpadFileForTesting(kBadFilePath);
  std::vector<uint64_t> addresses = {0x1010u, 0x1040u, 0x10d0u, 0x1140u};
  std::vector<std::vector<SymbolizedFrame>> frames =
      symbolizer.Symbolize("mapping", "build", 0, addresses);
  EXPECT_TRUE(frames.empty());
}

// To make it easy to read, each FUNC record is followed by two LINE records:
// one showing the start address of the ending instruction and one showing the
// address where the function ends.
constexpr char kTestFileContents[] =
    "MODULE mac x86_64 A68BC89F12C foo.so\n"
    "FUNC 1010 23 0 foo_foo()\n"
    "1031 2 39 4\n"
    "1033 0 0 0\n"
    "FUNC 1040 84 0 bar_bar_bar()\n"
    "10b6 e 44 5\n"
    "10c4 0 0 0\n"
    "FUNC 10d0 6b 0 foo::bar()\n"
    "1136 5 44 5\n"
    "113b 0 0 0\n"
    "FUNC 1140 6b 0 baz()\n"
    "114a 2 82 5\n"
    "114c 0 0 0\n";
constexpr ssize_t kTestFileLength = base::ArraySize(kTestFileContents);

TEST(BreakpadSymbolizerTest, SymbolFrames) {
  base::TempFile test_file = base::TempFile::Create();
  ASSERT_TRUE(*test_file);
  ssize_t written =
      base::WriteAll(test_file.fd(), kTestFileContents, kTestFileLength);
  ASSERT_EQ(written, kTestFileLength);
  constexpr char kTestDir[] = "Unused";
  BreakpadSymbolizer symbolizer(kTestDir);
  symbolizer.SetBreakpadFileForTesting(test_file.path());
  // The first 4 addresses are valid, while the last four, cannot be mapped to a
  // function because they are either too low, too large, or not mapped in any
  // function's range.
  std::vector<uint64_t> addresses = {0x1010u, 0x1040u, 0x10d0u, 0x1140u,
                                     0xeu,    0x1036u, 0x30d0u, 0x113eu};
  std::vector<std::vector<SymbolizedFrame>> frames =
      symbolizer.Symbolize("mapping", "build", 0, addresses);
  ASSERT_EQ(frames.size(), 8u);
  EXPECT_EQ(frames[0][0].function_name, "foo_foo()");
  EXPECT_EQ(frames[1][0].function_name, "bar_bar_bar()");
  EXPECT_EQ(frames[2][0].function_name, "foo::bar()");
  EXPECT_EQ(frames[3][0].function_name, "baz()");
  EXPECT_TRUE(frames[4][0].function_name.empty());
  EXPECT_TRUE(frames[5][0].function_name.empty());
  EXPECT_TRUE(frames[6][0].function_name.empty());
  EXPECT_TRUE(frames[7][0].function_name.empty());
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
