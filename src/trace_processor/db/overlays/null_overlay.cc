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

#include "src/trace_processor/db/overlays/null_overlay.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/db/overlays/types.h"

namespace perfetto {
namespace trace_processor {
namespace overlays {

using Range = RowMap::Range;

StorageRange NullOverlay::MapToStorageRange(TableRange t_range) const {
  uint32_t start = non_null_->CountSetBits(t_range.range.start);
  uint32_t end = non_null_->CountSetBits(t_range.range.end);

  return StorageRange(start, end);
}

TableRangeOrBitVector NullOverlay::MapToTableRangeOrBitVector(
    StorageRange s_range,
    OverlayOp op) const {
  PERFETTO_DCHECK(s_range.range.end <= non_null_->CountSetBits());

  BitVector range_to_bv(s_range.range.start, false);
  range_to_bv.Resize(s_range.range.end, true);

  return TableRangeOrBitVector(
      MapToTableBitVector(StorageBitVector{std::move(range_to_bv)}, op).bv);
}

TableBitVector NullOverlay::MapToTableBitVector(StorageBitVector s_bv,
                                                OverlayOp op) const {
  BitVector res = non_null_->Copy();
  res.UpdateSetBits(s_bv.bv);

  if (op != OverlayOp::kIsNull)
    return {std::move(res)};

  BitVector not_non_null = non_null_->Copy();
  not_non_null.Not();

  if (res.CountSetBits() == 0)
    return {std::move(not_non_null)};

  res.Or(not_non_null);
  return {std::move(res)};
}

BitVector NullOverlay::IsStorageLookupRequired(
    OverlayOp op,
    const TableIndexVector& t_iv) const {
  PERFETTO_DCHECK(t_iv.indices.size() <= non_null_->size());

  if (op != OverlayOp::kOther)
    return BitVector(t_iv.size(), false);

  BitVector in_storage(static_cast<uint32_t>(t_iv.indices.size()), false);

  // For each index in TableIndexVector check whether this index is in storage.
  for (uint32_t i = 0; i < t_iv.indices.size(); ++i) {
    if (non_null_->IsSet(t_iv.indices[i]))
      in_storage.Set(i);
  }

  return in_storage;
}

StorageIndexVector NullOverlay::MapToStorageIndexVector(
    TableIndexVector t_iv_with_idx_in_storage) const {
  PERFETTO_DCHECK(t_iv_with_idx_in_storage.indices.size() <=
                  non_null_->CountSetBits());

  std::vector<uint32_t> storage_index_vector;
  storage_index_vector.reserve(t_iv_with_idx_in_storage.indices.size());
  for (auto t_idx : t_iv_with_idx_in_storage.indices) {
    storage_index_vector.push_back(non_null_->CountSetBits(t_idx));
  }

  return StorageIndexVector({std::move(storage_index_vector)});
}

BitVector NullOverlay::IndexSearch(
    OverlayOp op,
    const TableIndexVector& t_iv_overlay_idx) const {
  if (op == OverlayOp::kOther)
    return BitVector(t_iv_overlay_idx.size(), false);

  BitVector res(static_cast<uint32_t>(t_iv_overlay_idx.indices.size()), false);
  if (op == OverlayOp::kIsNull) {
    for (uint32_t i = 0; i < res.size(); ++i) {
      if (!non_null_->IsSet(t_iv_overlay_idx.indices[i]))
        res.Set(i);
    }
    return res;
  }

  PERFETTO_DCHECK(op == OverlayOp::kIsNotNull);
  for (uint32_t i = 0; i < res.size(); ++i) {
    if (non_null_->IsSet(t_iv_overlay_idx.indices[i]))
      res.Set(i);
  }
  return res;
}

CostEstimatePerRow NullOverlay::EstimateCostPerRow(OverlayOp op) const {
  // TODO(b/283763282): Replace with benchmarked data.
  CostEstimatePerRow res;

  // Two |BitVector::CountSetBits| calls.
  res.to_storage_range = 100;

  // Cost of |BitVector::UpdateSetBits|
  res.to_table_bit_vector = 100;

  if (op == OverlayOp::kOther) {
    // Cost of |BitVector::IsSet| and |BitVector::Set|
    res.is_storage_search_required = 10;

    // Cost of iterating all set bits and looping the index vector divided by
    // number of indices.
    res.map_to_storage_index_vector = 100;

    // Won't be called.
    res.index_search = 0;
  } else {
    // Cost of creating trivial BitVector.
    res.is_storage_search_required = 0;

    // Won't be called
    res.map_to_storage_index_vector = 0;

    // Cost of calling |BitVector::IsSet|
    res.index_search = 10;
  }

  return res;
}

}  // namespace overlays
}  // namespace trace_processor
}  // namespace perfetto
