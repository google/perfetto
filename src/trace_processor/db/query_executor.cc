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

#include <array>
#include <cstddef>
#include <memory>
#include <numeric>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/containers/row_map.h"
#include "src/trace_processor/db/overlays/null_overlay.h"
#include "src/trace_processor/db/overlays/selector_overlay.h"
#include "src/trace_processor/db/overlays/storage_overlay.h"
#include "src/trace_processor/db/overlays/types.h"
#include "src/trace_processor/db/query_executor.h"
#include "src/trace_processor/db/storage/id_storage.h"
#include "src/trace_processor/db/storage/numeric_storage.h"
#include "src/trace_processor/db/storage/types.h"
#include "src/trace_processor/db/table.h"

namespace perfetto {
namespace trace_processor {

namespace {

using Range = RowMap::Range;
using OverlayOp = overlays::OverlayOp;
using StorageRange = overlays::StorageRange;
using TableRange = overlays::TableRange;
using Storage = storage::Storage;
using StorageOverlay = overlays::StorageOverlay;
using TableIndexVector = overlays::TableIndexVector;
using StorageIndexVector = overlays::StorageIndexVector;
using TableBitVector = overlays::TableBitVector;
using StorageBitVector = overlays::StorageBitVector;
using OverlaysVec = base::SmallVector<const overlays::StorageOverlay*,
                                      QueryExecutor::kMaxOverlayCount>;

// Helper struct to simplify operations on |global| and |current| sets of
// indices. Having this coupling enables efficient implementation of
// IndexedColumnFilter.
struct IndexFilterHelper {
  explicit IndexFilterHelper(std::vector<uint32_t> indices) {
    current_ = indices;
    global_ = std::move(indices);
  }

  // Removes pairs of elements that are not set in the |bv| and returns
  // Indices made of them.
  static std::pair<IndexFilterHelper, IndexFilterHelper> Partition(
      IndexFilterHelper indices,
      const BitVector& bv) {
    if (bv.CountSetBits() == 0) {
      return {IndexFilterHelper(), indices};
    }

    IndexFilterHelper set_partition;
    IndexFilterHelper non_set_partition;
    for (auto it = bv.IterateAllBits(); it; it.Next()) {
      uint32_t idx = it.index();
      if (it.IsSet()) {
        set_partition.PushBack({indices.current_[idx], indices.global_[idx]});
      } else {
        non_set_partition.PushBack(
            {indices.current_[idx], indices.global_[idx]});
      }
    }
    return {set_partition, non_set_partition};
  }

  // Removes pairs of elements that are not set in the |bv|. Returns count of
  // removed elements.
  uint32_t KeepAtSet(BitVector filter_nulls) {
    PERFETTO_CHECK(filter_nulls.size() == current_.size() ||
                   filter_nulls.CountSetBits() == 0);
    uint32_t count_removed =
        static_cast<uint32_t>(current_.size()) - filter_nulls.CountSetBits();

    uint32_t i = 0;
    auto filter = [&i, &filter_nulls](uint32_t) {
      return !filter_nulls.IsSet(i++);
    };

    auto current_it = std::remove_if(current_.begin(), current_.end(), filter);
    current_.erase(current_it, current_.end());

    i = 0;
    auto global_it = std::remove_if(global_.begin(), global_.end(), filter);
    global_.erase(global_it, global_.end());

    return count_removed;
  }

  std::vector<uint32_t>& current() { return current_; }

  std::vector<uint32_t>& global() { return global_; }

 private:
  IndexFilterHelper() = default;

  void PushBack(std::pair<uint32_t, uint32_t> cur_and_global_idx) {
    current_.push_back(cur_and_global_idx.first);
    global_.push_back(cur_and_global_idx.second);
  }

  std::vector<uint32_t> current_;
  std::vector<uint32_t> global_;
};
}  // namespace

void QueryExecutor::FilterColumn(const Constraint& c,
                                 const SimpleColumn& col,
                                 RowMap* rm) {
  if (rm->empty())
    return;

  if (col.sorted_intrinsically && c.op != FilterOp::kNe) {
    BinarySearch(c, col, rm);
    return;
  }

  uint32_t rm_size = rm->size();
  uint32_t rm_first = rm->Get(0);
  uint32_t rm_last = rm->Get(rm_size - 1);
  uint32_t range_size = rm_last - rm_first;

  // If the number of elements in the rowmap is small or the number of elements
  // is less than 1/10th of the range, use indexed filtering.
  // TODO(b/283763282): use Overlay estimations.
  if (rm->IsIndexVector() || rm_size < 1024 || rm_size * 10 < range_size) {
    *rm = IndexSearch(c, col, rm);
    return;
  }

  BitVector bv = LinearSearch(c, col, rm);
  if (rm->IsRange()) {
    // If |rm| is a range, the BitVector returned by LinearSearch perfectly
    // captures the previously filtered results already so does not need to
    // be intersected.
    *rm = RowMap(std::move(bv));
  } else if (rm->IsBitVector()) {
    // We need to reconcile our BitVector with |rm| to ensure that we don't
    // discard results from previous searches.
    rm->Intersect(RowMap(std::move(bv)));
  } else {
    // As we use |IsIndexVector()| above, to always use IndexSearch, we should
    // never hit this case.
    PERFETTO_FATAL("Should not happen");
  }
}

BitVector QueryExecutor::LinearSearch(const Constraint& c,
                                      const SimpleColumn& col,
                                      RowMap* rm) {
  // TODO(b/283763282): We should align these to word boundaries.
  TableRange table_range(rm->Get(0), rm->Get(rm->size() - 1) + 1);
  base::SmallVector<Range, kMaxOverlayCount> overlay_bounds;

  for (const auto& overlay : col.overlays) {
    StorageRange storage_range = overlay->MapToStorageRange(table_range);
    overlay_bounds.emplace_back(storage_range.range);
    table_range = TableRange(storage_range.range);
  }

  // Use linear search algorithm on storage.
  overlays::StorageBitVector filtered_storage{
      col.storage->LinearSearch(c.op, c.value, table_range.range)};

  for (uint32_t i = 0; i < col.overlays.size(); ++i) {
    uint32_t rev_i = static_cast<uint32_t>(col.overlays.size()) - 1 - i;
    TableBitVector mapped_to_table = col.overlays[rev_i]->MapToTableBitVector(
        std::move(filtered_storage), overlays::FilterOpToOverlayOp(c.op));
    filtered_storage = StorageBitVector({std::move(mapped_to_table.bv)});
  }
  return std::move(filtered_storage.bv);
}

void QueryExecutor::BinarySearch(const Constraint& c,
                                 const SimpleColumn& col,
                                 RowMap* rm) {
  // TODO(b/283763282): We should align these to word boundaries.
  TableRange table_range{Range(rm->Get(0), rm->Get(rm->size() - 1) + 1)};
  base::SmallVector<Range, kMaxOverlayCount> overlay_bounds;

  for (const auto& overlay : col.overlays) {
    StorageRange storage_range = overlay->MapToStorageRange(table_range);
    overlay_bounds.emplace_back(storage_range.range);
    table_range = TableRange({storage_range.range});
  }

  // Use binary search algorithm on storage.
  overlays::TableRangeOrBitVector res(
      col.storage->BinarySearchIntrinsic(c.op, c.value, table_range.range));

  OverlayOp op = overlays::FilterOpToOverlayOp(c.op);
  for (uint32_t i = 0; i < col.overlays.size(); ++i) {
    uint32_t rev_i = static_cast<uint32_t>(col.overlays.size()) - 1 - i;

    if (res.IsBitVector()) {
      TableBitVector t_bv = col.overlays[rev_i]->MapToTableBitVector(
          StorageBitVector{res.TakeIfBitVector()}, op);
      res.val = std::move(t_bv.bv);
    } else {
      res = col.overlays[rev_i]->MapToTableRangeOrBitVector(
          StorageRange(res.TakeIfRange()), op);
    }
  }

  if (res.IsBitVector()) {
    rm->Intersect(RowMap(res.TakeIfBitVector()));
    return;
  }

  rm->Intersect(RowMap(res.TakeIfRange().start, res.TakeIfRange().end));
}

RowMap QueryExecutor::IndexSearch(const Constraint& c,
                                  const SimpleColumn& col,
                                  RowMap* rm) {
  // Create outmost TableIndexVector.
  std::vector<uint32_t> table_indices;
  table_indices.reserve(rm->size());
  for (auto it = rm->IterateRows(); it; it.Next()) {
    table_indices.push_back(it.index());
  }

  // Datastructures for storing data across overlays.
  IndexFilterHelper to_filter(std::move(table_indices));
  std::vector<uint32_t> valid;
  uint32_t count_removed = 0;

  // Fetch the list of indices that require storage lookup and deal with all
  // of the indices that can be compared before it.
  OverlayOp op = overlays::FilterOpToOverlayOp(c.op);
  for (const auto& overlay : col.overlays) {
    BitVector partition =
        overlay->IsStorageLookupRequired(op, {to_filter.current()});

    // Most overlays don't require partitioning.
    if (partition.CountSetBits() == partition.size()) {
      to_filter.current() =
          overlay->MapToStorageIndexVector({to_filter.current()}).indices;
      continue;
    }

    // Separate indices that don't require storage lookup. Those can be dealt
    // with in each pass.
    auto [storage_lookup, no_storage_lookup] =
        IndexFilterHelper::Partition(to_filter, partition);
    to_filter = storage_lookup;

    // Erase the values which don't match the constraint and add the
    // remaining ones to the result.
    BitVector valid_bv =
        overlay->IndexSearch(op, {no_storage_lookup.current()});
    count_removed += no_storage_lookup.KeepAtSet(std::move(valid_bv));
    valid.insert(valid.end(), no_storage_lookup.global().begin(),
                 no_storage_lookup.global().end());

    // Update the current indices to the next storage overlay.
    to_filter.current() =
        overlay->MapToStorageIndexVector({to_filter.current()}).indices;
  }

  BitVector matched_in_storage = col.storage->IndexSearch(
      c.op, c.value, to_filter.current().data(),
      static_cast<uint32_t>(to_filter.current().size()));
  count_removed += to_filter.KeepAtSet(std::move(matched_in_storage));
  valid.insert(valid.end(), to_filter.global().begin(),
               to_filter.global().end());

  PERFETTO_CHECK(rm->size() == valid.size() + count_removed);

  std::sort(valid.begin(), valid.end());
  return RowMap(std::move(valid));
}

RowMap QueryExecutor::FilterLegacy(const Table* table,
                                   const std::vector<Constraint>& c_vec) {
  RowMap rm(0, table->row_count());
  for (const auto& c : c_vec) {
    const Column& col = table->columns()[c.col_idx];
    uint32_t column_size = col.IsId() ? col.overlay().row_map().output_size()
                                      : col.storage_base().size();

    // RowMap size
    bool use_legacy = rm.size() == 1;

    // Column types
    use_legacy = use_legacy || col.col_type() == ColumnType::kString ||
                 col.col_type() == ColumnType::kDummy;

    // Mismatched types
    use_legacy = use_legacy || (overlays::FilterOpToOverlayOp(c.op) ==
                                    overlays::OverlayOp::kOther &&
                                col.type() != c.value.type);

    // Specific column flags.
    use_legacy = use_legacy || col.IsDense() || col.IsSetId();

    // Non bit vector based selector
    use_legacy = use_legacy || (col.overlay().size() != column_size &&
                                !col.overlay().row_map().IsBitVector());

    // Extrinsically sorted columns
    use_legacy = use_legacy ||
                 (col.IsSorted() && col.overlay().row_map().IsIndexVector());

    if (use_legacy) {
      col.FilterInto(c.op, c.value, &rm);
      continue;
    }

    std::unique_ptr<Storage> storage;
    SimpleColumn s_col{OverlaysVec(), nullptr, col.IsSorted()};
    if (col.IsId()) {
      storage.reset(new storage::IdStorage(column_size));
    } else {
      storage.reset(new storage::NumericStorage(
          col.storage_base().data(), col.storage_base().non_null_size(),
          col.col_type()));
    }
    s_col.storage = storage.get();

    overlays::SelectorOverlay selector_overlay(
        col.overlay().row_map().GetIfBitVector());
    if (col.overlay().size() != column_size)
      s_col.overlays.emplace_back(&selector_overlay);

    BitVector null_bv;
    overlays::NullOverlay null_overlay(
        col.IsNullable() ? col.storage_base().bv() : &null_bv);
    if (col.IsNullable())
      s_col.overlays.emplace_back(&null_overlay);

    uint32_t pre_count = rm.size();
    FilterColumn(c, s_col, &rm);
    PERFETTO_DCHECK(rm.size() <= pre_count);
  }
  return rm;
}

}  // namespace trace_processor
}  // namespace perfetto
