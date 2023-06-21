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

#include "src/trace_processor/db/overlays/arrangement_overlay.h"
#include <iterator>
#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/db/overlays/types.h"

namespace perfetto {
namespace trace_processor {
namespace overlays {

using Range = RowMap::Range;

StorageRange ArrangementOverlay::MapToStorageRange(TableRange t_range) const {
  PERFETTO_CHECK(t_range.range.end <= arrangement_->size());
  const auto [min, max] =
      std::minmax_element(arrangement_->data() + t_range.range.start,
                          arrangement_->data() + t_range.range.end);

  return StorageRange(*min, *max + 1);
}

TableRangeOrBitVector ArrangementOverlay::MapToTableRangeOrBitVector(
    StorageRange s_range,
    OverlayOp) const {
  BitVector ret(static_cast<uint32_t>(arrangement_->size()), false);
  for (uint32_t i = 0; i < arrangement_->size(); ++i) {
    if (s_range.range.Contains((*arrangement_)[i]))
      ret.Set(i);
  }
  return TableRangeOrBitVector(std::move(ret));
}

TableBitVector ArrangementOverlay::MapToTableBitVector(StorageBitVector s_bv,
                                                       OverlayOp) const {
  BitVector::Builder builder(static_cast<uint32_t>(arrangement_->size()));
  uint32_t cur_idx = 0;

  // Fast path: we compare as many groups of 64 elements as we can.
  // This should be very easy for the compiler to auto-vectorize.
  uint32_t fast_path_elements = builder.BitsInCompleteWordsUntilFull();
  for (uint32_t i = 0; i < fast_path_elements; i += BitVector::kBitsInWord) {
    uint64_t word = 0;
    // This part should be optimised by SIMD and is expected to be fast.
    for (uint32_t k = 0; k < BitVector::kBitsInWord; ++k, ++cur_idx) {
      bool comp_result = s_bv.bv.IsSet((*arrangement_)[cur_idx]);
      word |= static_cast<uint64_t>(comp_result) << k;
    }
    builder.AppendWord(word);
  }

  // Slow path: we compare <64 elements and append to fill the Builder.
  uint32_t back_elements = builder.BitsUntilFull();
  for (uint32_t i = 0; i < back_elements; ++i, ++cur_idx) {
    builder.Append(s_bv.bv.IsSet((*arrangement_)[cur_idx]));
  }
  return TableBitVector{std::move(builder).Build()};
}

BitVector ArrangementOverlay::IsStorageLookupRequired(
    OverlayOp,
    const TableIndexVector& t_iv) const {
  return BitVector(t_iv.size(), true);
}

StorageIndexVector ArrangementOverlay::MapToStorageIndexVector(
    TableIndexVector t_iv) const {
  std::vector<uint32_t> ret;
  for (const auto& i : t_iv.indices) {
    ret.push_back((*arrangement_)[i]);
  }
  return StorageIndexVector{ret};
}

BitVector ArrangementOverlay::IndexSearch(OverlayOp,
                                          const TableIndexVector&) const {
  PERFETTO_FATAL("IndexSearch should not be called inside ArrangementOverlay");
}

CostEstimatePerRow ArrangementOverlay::EstimateCostPerRow(OverlayOp) const {
  CostEstimatePerRow estimate;
  // Cost of std::min and std::max
  estimate.to_storage_range = 20;
  // Free
  estimate.to_table_bit_vector = 0;
  // Cost of creating trivial vector of 1s
  estimate.is_storage_search_required = 0;
  // Cost of a lookup inside |arrangement_|
  estimate.map_to_storage_index_vector = 10;
  // Shouldn't be called
  estimate.index_search = 0;

  return estimate;
}

}  // namespace overlays
}  // namespace trace_processor
}  // namespace perfetto
