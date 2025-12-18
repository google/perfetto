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

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace protozero {

// Performs filtering of strings in an "iptables" style. See the comments in
// |TraceConfig.TraceFilter| for information on how this class works.
class StringFilter {
 public:
  // Bitmask for semantic types. Supports up to 128 semantic types (2 * 64).
  // Bit i in mask[j] set means semantic type i + j*64 is enabled.
  using SemanticTypeMask = std::array<uint64_t, 2>;

  // Maximum semantic type value supported.
  static constexpr size_t kSemanticTypeLimit =
      std::size(SemanticTypeMask()) * 64;

  // Returns a SemanticTypeMask with all bits set (applies to all types).
  static constexpr SemanticTypeMask AllSemanticTypes() {
    return {std::numeric_limits<uint64_t>::max(),
            std::numeric_limits<uint64_t>::max()};
  }

  enum class Policy : uint8_t {
    kMatchRedactGroups = 1,
    kAtraceMatchRedactGroups = 2,
    kMatchBreak = 3,
    kAtraceMatchBreak = 4,
    kAtraceRepeatedSearchRedactGroups = 5,
  };

  // Adds a new rule for filtering strings.
  //
  // If `name` is non-empty and a rule with the same name already exists, it
  // will be replaced; otherwise the rule is appended.
  //
  // `semantic_type_mask` is a bitmask indicating which semantic types this rule
  // applies to. Defaults to all bits set (applies to all semantic types).
  void AddRule(Policy policy,
               std::string_view pattern,
               std::string atrace_payload_starts_with,
               std::string name = {},
               SemanticTypeMask semantic_type_mask = AllSemanticTypes());

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
    // Bitmask of semantic types this rule applies to.
    SemanticTypeMask semantic_type_mask;
  };

  bool MaybeFilterInternal(char* ptr, size_t len, uint32_t semantic_type) const;

  // All rules, in the order they were added.
  std::vector<Rule> rules_;
};

}  // namespace protozero

#endif  // SRC_PROTOZERO_FILTERING_STRING_FILTER_H_
