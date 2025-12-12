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

#ifndef SRC_PROTOZERO_FILTERING_STRING_FILTER_H_
#define SRC_PROTOZERO_FILTERING_STRING_FILTER_H_

#include <cstdint>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace protozero {

// Performs filtering of strings in an "iptables" style. See the comments in
// |TraceConfig.TraceFilter| for information on how this class works.
class StringFilter {
 public:
  enum class Policy {
    kMatchRedactGroups = 1,
    kAtraceMatchRedactGroups = 2,
    kMatchBreak = 3,
    kAtraceMatchBreak = 4,
    kAtraceRepeatedSearchRedactGroups = 5,
  };

  // Adds a new rule for filtering strings. If |name| is non-empty and a rule
  // with the same name already exists, it will be replaced; otherwise the rule
  // is appended. |semantic_types| specifies which field types this rule applies
  // to; if empty, the rule applies to all fields.
  void AddRule(Policy policy,
               std::string_view pattern,
               std::string atrace_payload_starts_with,
               std::string name = {},
               std::vector<uint32_t> semantic_types = {});

  // Tries to filter the given string. Returns true if the string was modified
  // in any way, false otherwise. Uses semantic_type=0 (unspecified).
  bool MaybeFilter(char* ptr, size_t len) const {
    return MaybeFilter(ptr, len, /*semantic_type=*/0);
  }

  // Tries to filter the given string with a specific semantic type.
  // Only rules that match the semantic type (or have no type restriction)
  // are applied.
  bool MaybeFilter(char* ptr, size_t len, uint32_t semantic_type) const {
    if (len == 0 || rules_.empty()) {
      return false;
    }
    return MaybeFilterInternal(ptr, len, semantic_type);
  }

 private:
  struct Rule {
    Policy policy;
    std::regex pattern;
    std::string atrace_payload_starts_with;
    std::string name;
    std::vector<uint32_t> semantic_types;  // Empty means applies to all.
  };

  bool MaybeFilterInternal(char* ptr, size_t len, uint32_t semantic_type) const;

  std::vector<Rule> rules_;
};

}  // namespace protozero

#endif  // SRC_PROTOZERO_FILTERING_STRING_FILTER_H_
