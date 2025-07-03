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

#include "src/profiling/symbolizer/common.h"
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

// Helper to format the symbolized result for comparison.
std::string FormatFrames(const std::vector<SymbolizedFrame>& frames) {
  std::string result;
  for (const auto& frame : frames) {
    result += frame.function_name;
    result += "\n";
    result += frame.file_name;
    result += ":";
    result += std::to_string(frame.line);
    result += ":0\n";
  }
  return result;
}

// Tests symbolization for both a normal function and an inlined function.
TEST(LlvmSymbolizerTest, Symbolize) {
  // ust be updated if the binary is recompiled.
  constexpr uint64_t normal_function_address = 0x1130;
  constexpr uint64_t inlined_function_address = 0x1140;
  LlvmSymbolizer symbolizer;
  const std::vector<SymbolizationRequest> requests = {
      {"test/data/test_symbolizer_binary", normal_function_address},
      {"test/data/test_symbolizer_binary", inlined_function_address},
  };
  std::vector<std::vector<SymbolizedFrame>> results =
      symbolizer.SymbolizeBatch(requests);

  // Currently we ignore column numbers (not stored in the frame)
  ASSERT_EQ(FormatFrames(results[0]),
            "TestFunctionToSymbolize()\n"
            "/usr/local/test/test_symbolizer_binary.cc:3:0\n");
  ASSERT_EQ(FormatFrames(results[1]),
            "InlinedFunction()\n"
            "/usr/local/test/test_symbolizer_binary.cc:8:0\n"
            "TopLevelFunction()\n"
            "/usr/local/test/test_symbolizer_binary.cc:14:0\n");
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto

#endif  // PERFETTO_BUILDFLAG(PERFETTO_LLVM_SYMBOLIZER)
