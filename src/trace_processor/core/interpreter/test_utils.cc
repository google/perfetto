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

#include "src/trace_processor/core/interpreter/test_utils.h"

#include <cctype>
#include <string>

#include "perfetto/ext/base/string_utils.h"

namespace perfetto::trace_processor::core::interpreter {

std::string TrimSpacePerLine(const std::string& s) {
  std::string trimmed = base::TrimWhitespace(s);
  std::string result;
  result.reserve(trimmed.size());
  bool at_line_start = true;
  for (char c : trimmed) {
    if (c == '\n') {
      at_line_start = true;
      result += c;
    } else if (at_line_start && std::isspace(c)) {
      continue;
    } else {
      at_line_start = false;
      result += c;
    }
  }
  return result;
}

}  // namespace perfetto::trace_processor::core::interpreter
