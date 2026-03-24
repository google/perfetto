/*
 * Copyright (C) 2023 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_UTIL_REGEX_H_
#define SRC_TRACE_PROCESSOR_UTIL_REGEX_H_

#include <string>
#include <string_view>
#include <vector>

#include "perfetto/ext/base/regex.h"
#include "perfetto/ext/base/status_or.h"

namespace perfetto::trace_processor::regex {

constexpr bool IsRegexSupported() {
  return true;
}

class Regex {
 public:
  static base::StatusOr<Regex> Create(const char* pattern) {
    auto regex_or = base::Regex::Create(pattern);
    if (!regex_or.ok())
      return regex_or.status();
    return Regex(std::move(*regex_or));
  }

  bool Search(const char* s) const { return regex_.Search(s); }

  bool Submatch(const char* s, std::vector<std::string_view>& out) {
    return regex_.Submatch(s, out);
  }

  std::string Replace(const char* s, const char* repl) {
    return regex_.Replace(s, repl);
  }

 private:
  explicit Regex(base::Regex regex) : regex_(std::move(regex)) {}
  base::Regex regex_;
};

}  // namespace perfetto::trace_processor::regex

#endif  // SRC_TRACE_PROCESSOR_UTIL_REGEX_H_
