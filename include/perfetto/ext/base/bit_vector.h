/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_EXT_BASE_BIT_VECTOR_H_
#define INCLUDE_PERFETTO_EXT_BASE_BIT_VECTOR_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"

namespace perfetto {
namespace base {

// A growable set of small dense integers, with one bit per integer.
//
// Memory grows with the largest integer ever set, and it never shrinks.
// So use it only for indices that stay small, and use base::FlatSet for
// sparse or unbounded ids.
class BitVector {
 public:
  BitVector() = default;

  BitVector(const BitVector&) = delete;
  BitVector& operator=(const BitVector&) = delete;
  BitVector(BitVector&&) = delete;
  BitVector& operator=(BitVector&&) = delete;

  // Returns whether bit `i` is set, or false if `i` >= capacity().
  bool IsSet(size_t i) const {
    const size_t word = i / kBitsPerWord;
    return word < num_words_ && ((words_[word] >> (i % kBitsPerWord)) & 1u);
  }

  // Sets bit `i` and returns true if it was already set.
  // This is the only call that grows the storage, when `i` >= capacity().
  bool TestAndSet(size_t i) {
    const size_t word = i / kBitsPerWord;
    if (PERFETTO_UNLIKELY(word >= num_words_))
      Grow(word + 1);
    const uint64_t mask = uint64_t(1) << (i % kBitsPerWord);
    if (words_[word] & mask)
      return true;
    words_[word] |= mask;
    return false;
  }

  // Unsets bit `i`, or does nothing if `i` >= capacity().
  void Clear(size_t i) {
    const size_t word = i / kBitsPerWord;
    if (word >= num_words_)
      return;
    const uint64_t mask = uint64_t(1) << (i % kBitsPerWord);
    words_[word] &= ~mask;
  }

  // Unsets all the bits and keeps the allocation.
  void ClearAll() { std::fill_n(words_.get(), num_words_, uint64_t(0)); }

  // The number of bits that the current allocation holds.
  size_t capacity() const { return num_words_ * kBitsPerWord; }

 private:
  static constexpr size_t kBitsPerWord = 64;
  static constexpr size_t kMaxNumWords =
      std::numeric_limits<size_t>::max() / kBitsPerWord;

  // Doubles the word count so increasing indices need O(log n) reallocations.
  // Grows directly to a larger requested size when needed.
  PERFETTO_NO_INLINE void Grow(size_t min_num_words) {
    PERFETTO_CHECK(min_num_words <= kMaxNumWords);
    const size_t new_num_words =
        std::clamp(num_words_ * 2, min_num_words, kMaxNumWords);

    std::unique_ptr<uint64_t[]> words(new uint64_t[new_num_words]);
    std::copy_n(words_.get(), num_words_, words.get());
    std::fill(words.get() + num_words_, words.get() + new_num_words,
              uint64_t(0));
    words_ = std::move(words);
    num_words_ = new_num_words;
  }

  std::unique_ptr<uint64_t[]> words_;
  size_t num_words_ = 0;
};

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BASE_BIT_VECTOR_H_
