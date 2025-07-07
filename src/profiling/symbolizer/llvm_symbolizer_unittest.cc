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

#include "perfetto/base/build_config.h"
#include "test/gtest_and_gmock.h"

#if PERFETTO_BUILDFLAG(PERFETTO_LLVM_SYMBOLIZER)

#include <dlfcn.h>

#include <cinttypes>

#include "src/profiling/symbolizer/llvm_symbolizer.h"

namespace perfetto {
namespace profiling {
namespace {

// Tests that the LlvmSymbolizer can be constructed and destructed. This
// implicitly tests that the dynamic library can be loaded and the necessary
// symbols can be resolved.
TEST(LlvmSymbolizerTest, ConstructDestruct) {
  LlvmSymbolizer symbolizer;
}

// Tests symbolization for both a normal function and an inlined function.
// To update this test generate a new binary.
// To ensure proper symbolization using -g and -01 will force inline
// optimisations and debug information.
// To find the address of a function named TopLevelFunction you can use:
// nm ./binary | grep TopLevelFunction
TEST(LlvmSymbolizerTest, Symbolize) {
  // Must be updated if the binary is recompiled.
  constexpr uint64_t normal_function_address = 0x1130;
  constexpr uint64_t inlined_function_address = 0x1140;
  LlvmSymbolizer symbolizer;
  const std::vector<SymbolizationRequest> requests = {
      {"test/data/test_symbolizer_binary", normal_function_address},
      {"test/data/test_symbolizer_binary", inlined_function_address},
  };
  std::vector<std::vector<SymbolizedFrame>> results =
      symbolizer.SymbolizeBatch(requests);

  ASSERT_EQ(results.size(), 2u);

  ASSERT_EQ(results[0].size(), 1u);
  EXPECT_EQ(results[0][0].function_name, "TestFunctionToSymbolize()");
  EXPECT_EQ(results[0][0].file_name,
            "/usr/local/test/test_symbolizer_binary.cc");
  EXPECT_EQ(results[0][0].line, 3u);

  ASSERT_EQ(results[1].size(), 2u);
  EXPECT_EQ(results[1][0].function_name, "InlinedFunction()");
  EXPECT_EQ(results[1][0].file_name,
            "/usr/local/test/test_symbolizer_binary.cc");
  EXPECT_EQ(results[1][0].line, 8u);
  EXPECT_EQ(results[1][1].function_name, "TopLevelFunction()");
  EXPECT_EQ(results[1][1].file_name,
            "/usr/local/test/test_symbolizer_binary.cc");
  EXPECT_EQ(results[1][1].line, 14u);
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto

#endif  // PERFETTO_BUILDFLAG(PERFETTO_LLVM_SYMBOLIZER)
