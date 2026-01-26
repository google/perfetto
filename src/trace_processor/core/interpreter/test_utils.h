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

#ifndef SRC_TRACE_PROCESSOR_CORE_INTERPRETER_TEST_UTILS_H_
#define SRC_TRACE_PROCESSOR_CORE_INTERPRETER_TEST_UTILS_H_

#include <string>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::interpreter {

// Trims leading whitespace from each line in the string.
std::string TrimSpacePerLine(const std::string& s);

// Custom matcher that compares strings ignoring leading whitespace per line.
// Useful for comparing bytecode strings where indentation may vary.
MATCHER_P(EqualsIgnoringWhitespace,
          expected_str,
          "equals (ignoring leading whitespace per line)") {
  return ExplainMatchResult(
      testing::ResultOf(
          [](const std::string& s) { return TrimSpacePerLine(s); },
          testing::Eq(TrimSpacePerLine(expected_str))),
      arg, result_listener);
}

}  // namespace perfetto::trace_processor::core::interpreter

#endif  // SRC_TRACE_PROCESSOR_CORE_INTERPRETER_TEST_UTILS_H_
