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

#include "src/trace_processor/db/row_map.h"

namespace perfetto {
namespace trace_processor {

RowMap::RowMap(BitVector bit_vector)
    : compact_(true), bit_vector_(std::move(bit_vector)) {}

RowMap::RowMap(std::vector<uint32_t> vec)
    : compact_(false), index_vector_(std::move(vec)) {}

RowMap RowMap::Copy() const {
  return compact_ ? RowMap(bit_vector_.Copy()) : RowMap(index_vector_);
}

void RowMap::SelectRows(const RowMap& picker) {
  if (compact_ && picker.compact_) {
    bit_vector_.UpdateSetBits(picker.bit_vector_);
  } else if (compact_ && !picker.compact_) {
    index_vector_.resize(picker.index_vector_.size());
    for (uint32_t i = 0; i < picker.index_vector_.size(); ++i) {
      // TODO(lalitm): this is pretty inefficient.
      index_vector_[i] = bit_vector_.IndexOfNthSet(picker.index_vector_[i]);
    }
    compact_ = false;
    bit_vector_ = BitVector();
  } else if (!compact_ && picker.compact_) {
    uint32_t idx = 0;
    RemoveIf(
        [&idx, &picker](uint32_t) { return !picker.bit_vector_.IsSet(idx++); });
  } else /* (!compact_ && !picker.compact_) */ {
    std::vector<uint32_t> old_idx_vector = std::move(index_vector_);
    index_vector_ = std::vector<uint32_t>(picker.index_vector_.size());
    for (uint32_t i = 0; i < picker.index_vector_.size(); ++i) {
      PERFETTO_DCHECK(picker.index_vector_[i] < old_idx_vector.size());
      index_vector_[i] = old_idx_vector[picker.index_vector_[i]];
    }
  }
}

}  // namespace trace_processor
}  // namespace perfetto
