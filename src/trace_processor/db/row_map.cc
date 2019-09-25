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

RowMap RowMap::SelectRows(const RowMap& picker) const {
  if (compact_ && picker.compact_) {
    BitVector bv = bit_vector_.Copy();
    bv.UpdateSetBits(picker.bit_vector_);
    return RowMap(std::move(bv));
  } else if (compact_ && !picker.compact_) {
    std::vector<uint32_t> iv(picker.index_vector_.size());
    for (uint32_t i = 0; i < picker.index_vector_.size(); ++i) {
      // TODO(lalitm): this is pretty inefficient.
      iv[i] = bit_vector_.IndexOfNthSet(picker.index_vector_[i]);
    }
    return RowMap(std::move(iv));
  } else if (!compact_ && picker.compact_) {
    RowMap rm = Copy();
    uint32_t idx = 0;
    rm.RemoveIf(
        [&idx, &picker](uint32_t) { return !picker.bit_vector_.IsSet(idx++); });
    return rm;
  } else /* (!compact_ && !picker.compact_) */ {
    std::vector<uint32_t> iv(picker.index_vector_.size());
    for (uint32_t i = 0; i < picker.index_vector_.size(); ++i) {
      PERFETTO_DCHECK(picker.index_vector_[i] < index_vector_.size());
      iv[i] = index_vector_[picker.index_vector_[i]];
    }
    return RowMap(std::move(iv));
  }
}

}  // namespace trace_processor
}  // namespace perfetto
