/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "perfetto/base/string_utils.h"

#include "perfetto/base/logging.h"

namespace perfetto {
namespace base {

bool StartsWith(const std::string& str, const std::string& prefix) {
  return str.compare(0, prefix.length(), prefix) == 0;
}

bool EndsWith(const std::string& str, const std::string& suffix) {
  if (suffix.size() > str.size())
    return false;
  return str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

std::string Join(const std::vector<std::string>& parts,
                 const std::string& delim) {
  std::string acc;
  for (size_t i = 0; i < parts.size(); ++i) {
    acc += parts[i];
    if (i + 1 != parts.size()) {
      acc += delim;
    }
  }
  return acc;
}

std::vector<std::string> SplitString(const std::string& text,
                                     const std::string& delimiter) {
  PERFETTO_CHECK(!delimiter.empty());

  std::vector<std::string> output;
  size_t start = 0;
  size_t next;
  for (;;) {
    next = std::min(text.find(delimiter, start), text.size());
    output.emplace_back(&text[start], next - start);
    start = next + delimiter.size();
    if (start >= text.size())
      break;
  }
  return output;
}

}  // namespace base
}  // namespace perfetto
