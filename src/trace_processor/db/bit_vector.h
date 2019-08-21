/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_DB_BIT_VECTOR_H_
#define SRC_TRACE_PROCESSOR_DB_BIT_VECTOR_H_

#include <stdint.h>

#include <algorithm>
#include <vector>

#include "perfetto/base/logging.h"

namespace perfetto {
namespace trace_processor {

// A bitvector which compactly stores a vector of bools using a single bit
// for each bool.
// TODO(lalitm): currently this is just a thin wrapper around std::vector<bool>
// but in the future, we plan to add quite a few optimizations around ranges
// of set bits.
class BitVector {
 public:
  // Creates an empty bitvector.
  BitVector() = default;

  // Creates a bitvector of |count| size filled with |value|.
  BitVector(uint32_t count, bool value = false);

  BitVector(BitVector&&) noexcept = default;
  BitVector& operator=(BitVector&&) = default;

  // Create a copy of the bitvector.
  BitVector Copy() const;

  // Returns the size of the bitvector.
  uint32_t size() const { return static_cast<uint32_t>(inner_.size()); }

  // Returns whether the bit at |idx| is set.
  bool IsSet(uint32_t idx) const {
    PERFETTO_DCHECK(idx < size());
    return inner_[idx];
  }

  // Returns the index of the next set bit at or after index |idx|.
  // If there is no other set bits, returns |size()|.
  uint32_t NextSet(uint32_t idx) const {
    PERFETTO_DCHECK(idx <= inner_.size());
    auto it = std::find(inner_.begin() + static_cast<ptrdiff_t>(idx),
                        inner_.end(), true);
    return static_cast<uint32_t>(std::distance(inner_.begin(), it));
  }

  // Returns the number of set bits in the bitvector.
  uint32_t GetNumBitsSet() const { return GetNumBitsSet(size()); }

  // Returns the number of set bits between the start of the bitvector
  // (inclusive) and the index |end| (exclusive).
  uint32_t GetNumBitsSet(uint32_t end) const {
    return static_cast<uint32_t>(std::count(
        inner_.begin(), inner_.begin() + static_cast<ptrdiff_t>(end), true));
  }

  // Returns the index of the |n|'th set bit.
  uint32_t IndexOfNthSet(uint32_t n) const {
    // TODO(lalitm): improve the performance of this method by investigating
    // AVX instructions.
    uint32_t offset = 0;
    for (uint32_t i = NextSet(0); i < size(); i = NextSet(i + 1), ++offset) {
      if (offset == n)
        return i;
    }
    PERFETTO_FATAL("Index out of bounds");
  }

  // Sets the value at index |idx| to |value|.
  void Set(uint32_t idx, bool value) {
    PERFETTO_DCHECK(idx < size());
    inner_[idx] = value;
  }

  // Appends |value| to the bitvector.
  void Append(bool value) { inner_.push_back(value); }

  // Resizes the BitVector to the given |size|.
  // Truncates the BitVector if |size| < |size()| or fills the new space with
  // |value| if |size| > |size()|.
  void Resize(uint32_t size, bool value = false) { inner_.resize(size, value); }

  // Updates the ith set bit of this bitvector with the value of
  // |other.IsSet(i)|.
  //
  // This is the best way to batch update all the bits which are set; for
  // example when filtering rows, we want to filter all rows which are currently
  // included but ignore rows which have already been excluded.
  //
  // For example suppose the following:
  // this:  1 1 0 0 1 0 1
  // other: 0 1 1 0
  // This will change this to the following:
  // this:  0 1 0 0 1 0 0
  // TODO(lalitm): investigate whether we should just change this to And.
  void UpdateSetBits(const BitVector& other) {
    PERFETTO_DCHECK(other.size() == GetNumBitsSet());

    uint32_t offset = 0;
    for (uint32_t i = NextSet(0); i < size(); i = NextSet(i + 1), ++offset) {
      if (!other.IsSet(offset))
        Set(i, false);
    }
  }

 private:
  BitVector(std::vector<bool>);

  BitVector(const BitVector&) = delete;
  BitVector& operator=(const BitVector&) = delete;

  std::vector<bool> inner_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DB_BIT_VECTOR_H_
