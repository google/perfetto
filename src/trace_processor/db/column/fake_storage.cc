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

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/db/column/data_layer.h"
#include "src/trace_processor/db/column/types.h"

namespace perfetto::trace_processor::column {

FakeStorageChain::FakeStorageChain(uint32_t size,
                                   SearchStrategy strategy,
                                   Range range,
                                   BitVector bv)
    : size_(size),
      strategy_(strategy),
      range_(range),
      bit_vector_(std::move(bv)) {}

SingleSearchResult FakeStorageChain::SingleSearch(FilterOp,
                                                  SqlValue,
                                                  uint32_t i) const {
  switch (strategy_) {
    case kAll:
      return SingleSearchResult::kMatch;
    case kNone:
      return SingleSearchResult::kNoMatch;
    case kBitVector:
      return bit_vector_.IsSet(i) ? SingleSearchResult::kMatch
                                  : SingleSearchResult::kNoMatch;
    case kRange:
      return range_.Contains(i) ? SingleSearchResult::kMatch
                                : SingleSearchResult::kNoMatch;
  }
  PERFETTO_FATAL("For GCC");
}

UniqueSearchResult FakeStorageChain::UniqueSearch(FilterOp,
                                                  SqlValue,
                                                  uint32_t* i) const {
  switch (strategy_) {
    case kAll:
      if (size_ != 1) {
        return UniqueSearchResult::kNeedsFullSearch;
      }
      *i = 0;
      return UniqueSearchResult::kMatch;
    case kNone:
      return UniqueSearchResult::kNoMatch;
    case kBitVector:
      if (bit_vector_.CountSetBits() != 1) {
        return UniqueSearchResult::kNeedsFullSearch;
      }
      *i = bit_vector_.IndexOfNthSet(0);
      return UniqueSearchResult::kMatch;
    case kRange:
      if (range_.size() != 1) {
        return UniqueSearchResult::kNeedsFullSearch;
      }
      *i = range_.start;
      return UniqueSearchResult::kMatch;
  }
  PERFETTO_FATAL("For GCC");
}

SearchValidationResult FakeStorageChain::ValidateSearchConstraints(
    FilterOp,
    SqlValue) const {
  return SearchValidationResult::kOk;
}

RangeOrBitVector FakeStorageChain::SearchValidated(FilterOp,
                                                   SqlValue,
                                                   Range in) const {
  switch (strategy_) {
    case kAll:
      return RangeOrBitVector(in);
    case kNone:
      return RangeOrBitVector(Range());
    case kRange:
      return RangeOrBitVector(Range(std::max(in.start, range_.start),
                                    std::min(in.end, range_.end)));
    case kBitVector: {
      BitVector intersection = bit_vector_.IntersectRange(in.start, in.end);
      intersection.Resize(in.end, false);
      return RangeOrBitVector(std::move(intersection));
    }
  }
  PERFETTO_FATAL("For GCC");
}

RangeOrBitVector FakeStorageChain::IndexSearchValidated(FilterOp,
                                                        SqlValue,
                                                        Indices indices) const {
  switch (strategy_) {
    case kAll:
      return RangeOrBitVector(Range(0, indices.size));
    case kNone:
      return RangeOrBitVector(Range());
    case kRange:
    case kBitVector: {
      BitVector::Builder builder(indices.size);
      for (const uint32_t* it = indices.data; it != indices.data + indices.size;
           ++it) {
        bool in_range = strategy_ == kRange && range_.Contains(*it);
        bool in_bv = strategy_ == kBitVector && bit_vector_.IsSet(*it);
        builder.Append(in_range || in_bv);
      }
      return RangeOrBitVector(std::move(builder).Build());
    }
  }
  PERFETTO_FATAL("For GCC");
}

Range FakeStorageChain::OrderedIndexSearchValidated(FilterOp,
                                                    SqlValue,
                                                    Indices indices) const {
  if (strategy_ == kAll) {
    return {0, indices.size};
  }

  if (strategy_ == kNone) {
    return {};
  }

  if (strategy_ == kRange) {
    // We are looking at intersection of |range_| and |indices_|.
    const uint32_t* first_in_range = std::partition_point(
        indices.data, indices.data + indices.size,
        [this](uint32_t i) { return !range_.Contains(i); });
    const uint32_t* first_outside_range =
        std::partition_point(first_in_range, indices.data + indices.size,
                             [this](uint32_t i) { return range_.Contains(i); });
    return {static_cast<uint32_t>(std::distance(indices.data, first_in_range)),
            static_cast<uint32_t>(
                std::distance(indices.data, first_outside_range))};
  }

  PERFETTO_DCHECK(strategy_ == kBitVector);
  // We are looking at intersection of |range_| and |bit_vector_|.
  const uint32_t* first_set = std::partition_point(
      indices.data, indices.data + indices.size,
      [this](uint32_t i) { return !bit_vector_.IsSet(i); });
  const uint32_t* first_non_set =
      std::partition_point(first_set, indices.data + indices.size,
                           [this](uint32_t i) { return bit_vector_.IsSet(i); });
  return {static_cast<uint32_t>(std::distance(indices.data, first_set)),
          static_cast<uint32_t>(std::distance(indices.data, first_non_set))};
}

void FakeStorageChain::StableSort(SortToken*, SortToken*, SortDirection) const {
  PERFETTO_FATAL("Not implemented");
}

void FakeStorageChain::Serialize(StorageProto*) const {
  // FakeStorage doesn't really make sense to serialize.
  PERFETTO_FATAL("Not implemented");
}

}  // namespace perfetto::trace_processor::column
