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

#include "src/trace_processor/db/bit_vector.h"

#include "src/trace_processor/db/bit_vector_iterators.h"

namespace perfetto {
namespace trace_processor {

BitVector::BitVector() = default;

BitVector::BitVector(uint32_t count, bool value) {
  Resize(count, value);
}

BitVector::BitVector(std::vector<Block> blocks,
                     std::vector<uint32_t> counts,
                     uint32_t size)
    : size_(size), counts_(std::move(counts)), blocks_(std::move(blocks)) {}

BitVector BitVector::Copy() const {
  return BitVector(blocks_, counts_, size_);
}

BitVector::AllBitsIterator BitVector::IterateAllBits() const {
  return AllBitsIterator(this);
}

void BitVector::UpdateSetBits(const BitVector& other) {
  PERFETTO_DCHECK(other.size() == GetNumBitsSet());

  // Go through each set bit and if |other| has it unset, then unset the
  // bit taking care to update the index we consider to take into account
  // the bits we just unset.
  // TODO(lalitm): we add a set bits iterator implementation to remove this
  // inefficient loop.
  uint32_t removed = 0;
  for (auto it = other.IterateAllBits(); it; it.Next()) {
    if (!it.IsSet()) {
      Clear(IndexOfNthSet(it.index() - removed++));
    }
  }
}

}  // namespace trace_processor
}  // namespace perfetto
