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

#include "src/trace_processor/db/column/fake_storage.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/containers/row_map.h"
#include "src/trace_processor/db/column/column.h"
#include "src/trace_processor/db/column/types.h"

namespace perfetto {
namespace trace_processor {
namespace column {

FakeStorage::FakeStorage(uint32_t size, SearchStrategy strategy)
    : size_(size), strategy_(strategy) {}

SearchValidationResult FakeStorage::ValidateSearchConstraints(SqlValue,
                                                              FilterOp) const {
  return SearchValidationResult::kOk;
}

RangeOrBitVector FakeStorage::Search(FilterOp, SqlValue, Range in) const {
  switch (strategy_) {
    case kAll:
      return RangeOrBitVector(in);
    case kNone:
      return RangeOrBitVector(Range());
    case kRange:
      return RangeOrBitVector(Range(std::max(in.start, range_.start),
                                    std::min(in.end, range_.end)));
    case kBitVector:
      return RangeOrBitVector{bit_vector_.IntersectRange(in.start, in.end)};
  }
  PERFETTO_FATAL("For GCC");
}

RangeOrBitVector FakeStorage::IndexSearch(FilterOp,
                                          SqlValue,
                                          uint32_t* indices,
                                          uint32_t indices_size,
                                          bool) const {
  switch (strategy_) {
    case kAll:
      return RangeOrBitVector(Range(0, indices_size));
    case kNone:
      return RangeOrBitVector(Range());
    case kRange:
    case kBitVector: {
      BitVector::Builder builder(indices_size);
      for (uint32_t* it = indices; it != indices + indices_size; ++it) {
        bool in_range = strategy_ == kRange && range_.Contains(*it);
        bool in_bv = strategy_ == kBitVector && bit_vector_.IsSet(*it);
        builder.Append(in_range || in_bv);
      }
      return RangeOrBitVector(std::move(builder).Build());
    }
  }
  PERFETTO_FATAL("For GCC");
}

void FakeStorage::StableSort(uint32_t*, uint32_t) const {
  // TODO(b/307482437): Implement.
  PERFETTO_FATAL("Not implemented");
}

void FakeStorage::Sort(uint32_t*, uint32_t) const {
  // TODO(b/307482437): Implement.
  PERFETTO_FATAL("Not implemented");
}

void FakeStorage::Serialize(StorageProto*) const {
  // FakeStorage doesn't really make sense to serialize.
  PERFETTO_FATAL("Not implemented");
}

}  // namespace column
}  // namespace trace_processor
}  // namespace perfetto
