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
#include <limits>

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
  const std::vector<::SymbolizationRequest> requests = {
      {"test/data/test_symbolizer_binary", std::numeric_limits<uint32_t>::max(),
       normal_function_address},
      {"test/data/test_symbolizer_binary", std::numeric_limits<uint32_t>::max(),
       inlined_function_address},
  };
  SymbolizationResultBatch result_batch = symbolizer.SymbolizeBatch(requests);

  ASSERT_EQ(result_batch.size(), 2u);

  // Check the first request's result (normal function)
  auto res0 = result_batch.GetFramesForRequest(0);
  ASSERT_EQ(res0.second, 1u);
  const ::LlvmSymbolizedFrame* frames0 = res0.first;
  EXPECT_EQ(base::StringView(frames0[0].function_name),
            base::StringView("TestFunctionToSymbolize()"));
  EXPECT_EQ(base::StringView(frames0[0].file_name),
            base::StringView("/usr/local/test/test_symbolizer_binary.cc"));
  EXPECT_EQ(frames0[0].line_number, 3u);

  // Check the second request's result (inlined function)
  auto res1 = result_batch.GetFramesForRequest(1);
  ASSERT_EQ(res1.second, 2u);
  const ::LlvmSymbolizedFrame* frames1 = res1.first;
  EXPECT_EQ(base::StringView(frames1[0].function_name),
            base::StringView("InlinedFunction()"));
  EXPECT_EQ(base::StringView(frames1[0].file_name),
            base::StringView("/usr/local/test/test_symbolizer_binary.cc"));
  EXPECT_EQ(frames1[0].line_number, 8u);
  EXPECT_EQ(base::StringView(frames1[1].function_name),
            base::StringView("TopLevelFunction()"));
  EXPECT_EQ(base::StringView(frames1[1].file_name),
            base::StringView("/usr/local/test/test_symbolizer_binary.cc"));
  EXPECT_EQ(frames1[1].line_number, 14u);
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto

#endif  // PERFETTO_BUILDFLAG(PERFETTO_LLVM_SYMBOLIZER)
