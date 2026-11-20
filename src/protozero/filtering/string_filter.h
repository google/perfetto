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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/regex.h"
#include "perfetto/public/compiler.h"

namespace protozero {

// Performs filtering of strings in an "iptables" style. See the comments in
// |TraceConfig.TraceFilter| for information on how this class works.
class StringFilter {
 public:
  // Bitmask for semantic types. Supports up to 128 semantic types.
  class SemanticTypeMask {
   public:
    using Word = uint64_t;
    static constexpr size_t kBitsPerWord = sizeof(Word) * 8;
    static constexpr size_t kLimit = 128;

    // Returns a SemanticTypeMask with only bit 0 set (UNSPECIFIED only).
    // UNSPECIFIED is its own category and only matches if bit 0 is set.
    static constexpr SemanticTypeMask Unspecified() {
      return SemanticTypeMask(Word(1), Word(0));
    }

    // Creates a SemanticTypeMask from raw word values (for testing).
    static constexpr SemanticTypeMask FromWords(Word w0, Word w1) {
      return SemanticTypeMask(w0, w1);
    }

    // Default constructor: no bits set.
    constexpr SemanticTypeMask() = default;

    // Sets the bit for |semantic_type|.
    PERFETTO_ALWAYS_INLINE void Set(uint32_t semantic_type) {
      PERFETTO_DCHECK(semantic_type < kLimit);
      uint32_t word_index = semantic_type / kBitsPerWord;
      uint32_t bit_index = semantic_type % kBitsPerWord;
      words_[word_index] |= Word(1) << bit_index;
    }

    // Returns true if the bit for |semantic_type| is set.
    PERFETTO_ALWAYS_INLINE bool IsSet(uint32_t semantic_type) const {
      // If beyond supported range, return true (safe default: apply rule).
      if (PERFETTO_UNLIKELY(semantic_type >= kLimit)) {
        return true;
      }
      uint32_t word_index = semantic_type / kBitsPerWord;
      uint32_t bit_index = semantic_type % kBitsPerWord;
      return (words_[word_index] & (Word(1) << bit_index)) != 0;
    }

    // Sets all bits present in |other| on this mask in place.
    PERFETTO_ALWAYS_INLINE SemanticTypeMask& operator|=(
        const SemanticTypeMask& other) {
      words_[0] |= other.words_[0];
      words_[1] |= other.words_[1];
      return *this;
    }

   private:
    constexpr SemanticTypeMask(Word w0, Word w1) : words_{w0, w1} {}

    std::array<Word, 2> words_ = {};
  };

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
  // applies to. UNSPECIFIED (0) is its own category and only matches if bit 0
  // is explicitly set. Defaults to matching only UNSPECIFIED.
  void AddRule(
      Policy policy,
      std::string_view pattern,
      std::string atrace_payload_starts_with,
      std::string name = {},
      SemanticTypeMask semantic_type_mask = SemanticTypeMask::Unspecified());

  // Tries to filter the given string. Returns true if the string was modified
  // in any way, false otherwise. Uses semantic_type=0 (unspecified).
  bool MaybeFilter(char* ptr, size_t len) const {
    return MaybeFilter(ptr, len, /*semantic_type=*/0);
  }

  // Tries to filter the given string with a specific semantic type.
  // Only rules whose semantic_type_mask covers |semantic_type| are applied.
  bool MaybeFilter(char* ptr, size_t len, uint32_t semantic_type) const {
    // Fast path: skip MaybeFilterInternal when zero configured rules cover
    // |semantic_type|.
    if (len == 0 || !active_semantic_types_.IsSet(semantic_type)) {
      return false;
    }
    return MaybeFilterInternal(ptr, len, semantic_type);
  }

 private:
  struct Rule {
    Policy policy;
    std::optional<perfetto::base::CopyableRegex> pattern;
    std::string atrace_payload_starts_with;
    std::string name;
    SemanticTypeMask semantic_type_mask = SemanticTypeMask::Unspecified();
  };

  bool MaybeFilterInternal(char* ptr, size_t len, uint32_t semantic_type) const;
  void RecomputeActiveSemanticTypes();

  // All rules, in the order they were added.
  std::vector<Rule> rules_;

  // Bitwise union of |semantic_type_mask| across all entries in |rules_|.
  // Allows MaybeFilter() to skip MaybeFilterInternal() in O(1) when zero
  // configured rules cover a field's semantic type.
  SemanticTypeMask active_semantic_types_;

  // Reusable buffer for regex match groups, avoiding heap allocations on each
  // filter call.
  mutable std::vector<std::string_view> matches_;
};

}  // namespace protozero

#endif  // SRC_PROTOZERO_FILTERING_STRING_FILTER_H_
