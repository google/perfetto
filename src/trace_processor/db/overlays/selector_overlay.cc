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

#include "src/trace_processor/db/overlays/selector_overlay.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/db/overlays/types.h"

namespace perfetto {
namespace trace_processor {
namespace overlays {

using Range = RowMap::Range;

StorageRange SelectorOverlay::MapToStorageRange(TableRange t_range) const {
  // Table data is smaller than Storage, so we need to expand the data.
  return StorageRange{
      Range(selected_->IndexOfNthSet(t_range.range.start),
            selected_->IndexOfNthSet(t_range.range.end - 1) + 1)};
}

TableRangeOrBitVector SelectorOverlay::MapToTableRangeOrBitVector(
    StorageRange s_range,
    OverlayOp) const {
  if (s_range.range.size() == 0)
    return TableRangeOrBitVector(Range());

  uint32_t start = selected_->CountSetBits(s_range.range.start);
  uint32_t end = selected_->CountSetBits(s_range.range.end);

  return TableRangeOrBitVector(Range(start, end));
}

TableBitVector SelectorOverlay::MapToTableBitVector(StorageBitVector s_bv,
                                                    OverlayOp) const {
  PERFETTO_DCHECK(s_bv.bv.size() <= selected_->size());
  BitVector res(selected_->CountSetBits());
  // TODO(b/283763282): Implement this variation of |UpdateSetBits| in
  // BitVector.
  for (auto it = selected_->IterateSetBits(); it && it.index() < s_bv.bv.size();
       it.Next()) {
    if (s_bv.bv.IsSet(it.index()))
      res.Set(it.ordinal());
  }
  return TableBitVector({std::move(res)});
}

BitVector SelectorOverlay::IsStorageLookupRequired(
    OverlayOp,
    const TableIndexVector& t_iv) const {
  return BitVector(static_cast<uint32_t>(t_iv.indices.size()), true);
}

StorageIndexVector SelectorOverlay::MapToStorageIndexVector(
    TableIndexVector t_iv) const {
  PERFETTO_DCHECK(t_iv.indices.empty() ||
                  *std::max_element(t_iv.indices.begin(), t_iv.indices.end()) <=
                      selected_->size());
  // To go from TableIndexVector to StorageIndexVector we need to find index in
  // |selector_| by looking only into set bits.
  std::vector<uint32_t> s_iv;
  s_iv.reserve(t_iv.indices.size());
  for (auto t_idx : t_iv.indices) {
    s_iv.push_back(selected_->IndexOfNthSet(t_idx));
  }

  return StorageIndexVector({std::move(s_iv)});
}

BitVector SelectorOverlay::IndexSearch(OverlayOp,
                                       const TableIndexVector&) const {
  // |t_iv| doesn't contain any values that are null in |selected_| as other
  // overlays are not able to access them. This function should not be called.
  PERFETTO_FATAL("Should not be called in SelectorOverlay.");
}

CostEstimatePerRow SelectorOverlay::EstimateCostPerRow(OverlayOp) const {
  CostEstimatePerRow estimate;
  // Cost of two |IndexOfNthSet|
  estimate.to_storage_range = 20;
  // Cost of iterating over all selected bits and calling |IsSet| each time (and
  // |Set| if true)
  estimate.to_table_bit_vector = 100;
  // Cost of creating trivial vector of 1s
  estimate.is_storage_search_required = 0;
  // Cost of |IndexOfNthSet| for each row
  estimate.map_to_storage_index_vector = 10;
  // Shouldn't be called
  estimate.index_search = 0;

  return estimate;
}

}  // namespace overlays
}  // namespace trace_processor
}  // namespace perfetto
