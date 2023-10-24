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
#include "src/trace_processor/db/overlays/arrangement_overlay.h"
#include "src/trace_processor/db/overlays/null_overlay.h"
#include "src/trace_processor/db/overlays/selector_overlay.h"
#include "src/trace_processor/db/overlays/storage_overlay.h"
#include "src/trace_processor/db/overlays/types.h"
#include "src/trace_processor/db/query_executor.h"
#include "src/trace_processor/db/storage/dummy_storage.h"
#include "src/trace_processor/db/storage/id_storage.h"
#include "src/trace_processor/db/storage/numeric_storage.h"
#include "src/trace_processor/db/storage/string_storage.h"
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
    PERFETTO_DCHECK(filter_nulls.size() == current_.size() ||
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

  uint32_t rm_size = rm->size();
  uint32_t rm_first = rm->Get(0);
  uint32_t rm_last = rm->Get(rm_size - 1);
  uint32_t range_size = rm_last - rm_first;

  // If the number of elements in the rowmap is small or the number of elements
  // is less than 1/10th of the range, use indexed filtering.
  // TODO(b/283763282): use Overlay estimations.
  bool disallows_index_search = rm->IsRange();
  bool prefers_index_search =
      rm->IsIndexVector() || rm_size < 1024 || rm_size * 10 < range_size;
  if (!disallows_index_search && prefers_index_search) {
    *rm = IndexSearch(c, col, rm);
    return;
  }
  LinearSearch(c, col, rm);
}

void QueryExecutor::LinearSearch(const Constraint& c,
                                 const SimpleColumn& col,
                                 RowMap* rm) {
  // TODO(b/283763282): Align these to word boundaries.
  TableRange bounds{Range(rm->Get(0), rm->Get(rm->size() - 1) + 1)};

  // Translate the bounds to the storage level.
  for (const auto& overlay : col.overlays) {
    bounds = TableRange({overlay->MapToStorageRange(bounds).range});
  }

  // Search the storage.
  overlays::TableRangeOrBitVector res(
      col.storage->Search(c.op, c.value, bounds.range));

  // Translate the result to global level.
  OverlayOp op = overlays::FilterOpToOverlayOp(c.op);
  for (uint32_t i = 0; i < col.overlays.size(); ++i) {
    uint32_t rev_i = static_cast<uint32_t>(col.overlays.size()) - 1 - i;

    if (res.IsBitVector()) {
      TableBitVector t_bv = col.overlays[rev_i]->MapToTableBitVector(
          StorageBitVector{std::move(res).TakeIfBitVector()}, op);
      res.val = RangeOrBitVector(std::move(t_bv.bv));
    } else {
      res = col.overlays[rev_i]->MapToTableRangeOrBitVector(
          StorageRange(std::move(res).TakeIfRange()), op);
    }
  }

  if (rm->IsRange()) {
    if (res.IsRange()) {
      Range range = std::move(res).TakeIfRange();
      *rm = RowMap(range.start, range.end);
    } else {
      // The BitVector was already limited on the RowMap when created, so we can
      // take it as it is.
      *rm = RowMap(std::move(res).TakeIfBitVector());
    }
    return;
  }

  if (res.IsRange()) {
    Range range = std::move(res).TakeIfRange();
    rm->Intersect(RowMap(range.start, range.end));
    return;
  }
  rm->Intersect(RowMap(std::move(res).TakeIfBitVector()));
}

RowMap QueryExecutor::IndexSearch(const Constraint& c,
                                  const SimpleColumn& col,
                                  RowMap* rm) {
  // Create outmost TableIndexVector.
  std::vector<uint32_t> table_indices = std::move(*rm).TakeAsIndexVector();

  // Datastructures for storing data across overlays.
  IndexFilterHelper to_filter(std::move(table_indices));
  std::vector<uint32_t> matched;
  uint32_t count_removed = 0;
  uint32_t count_starting_indices =
      static_cast<uint32_t>(to_filter.current().size());

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
    matched.insert(matched.end(), no_storage_lookup.global().begin(),
                   no_storage_lookup.global().end());

    // Update the current indices to the next storage overlay.
    to_filter.current() =
        overlay->MapToStorageIndexVector({to_filter.current()}).indices;
  }

  RangeOrBitVector matched_in_storage = col.storage->IndexSearch(
      c.op, c.value, to_filter.current().data(),
      static_cast<uint32_t>(to_filter.current().size()));

  // TODO(b/283763282): Remove after implementing extrinsic binary search.
  PERFETTO_DCHECK(matched_in_storage.IsBitVector());

  count_removed +=
      to_filter.KeepAtSet(std::move(matched_in_storage).TakeIfBitVector());
  matched.insert(matched.end(), to_filter.global().begin(),
                 to_filter.global().end());

  PERFETTO_CHECK(count_starting_indices == matched.size() + count_removed);

  std::sort(matched.begin(), matched.end());
  return RowMap(std::move(matched));
}

RowMap QueryExecutor::FilterLegacy(const Table* table,
                                   const std::vector<Constraint>& c_vec) {
  RowMap rm(0, table->row_count());
  for (const auto& c : c_vec) {
    const Column& col = table->columns()[c.col_idx];
    uint32_t column_size =
        col.IsId() ? col.overlay().row_map().Max() : col.storage_base().size();

    // RowMap size
    bool use_legacy = rm.size() <= 1;

    // Rare cases where we have a range which doesn't match the size of the
    // column.
    use_legacy = use_legacy || (col.overlay().size() != column_size &&
                                col.overlay().row_map().IsRange());

    // Rare cases where string columns can be sorted.
    use_legacy =
        use_legacy || (col.IsSorted() && col.col_type() == ColumnType::kString);

    // Mismatched types
    use_legacy = use_legacy || (overlays::FilterOpToOverlayOp(c.op) ==
                                    overlays::OverlayOp::kOther &&
                                col.type() != c.value.type);

    // Specific column flags.
    use_legacy = use_legacy || col.IsDense() || col.IsSetId();

    // Extrinsically sorted columns
    use_legacy = use_legacy ||
                 (col.IsSorted() && col.overlay().row_map().IsIndexVector());

    if (use_legacy) {
      col.FilterInto(c.op, c.value, &rm);
      continue;
    }

    // String columns are inherently nullable: null values are signified with
    // Id::Null().
    PERFETTO_CHECK(
        !(col.col_type() == ColumnType::kString && col.IsNullable()));

    SimpleColumn s_col{OverlaysVec(), nullptr};

    // Create storage
    std::unique_ptr<Storage> storage;
    switch (col.col_type()) {
      case ColumnType::kDummy:
        storage.reset(new storage::DummyStorage());
        break;
      case ColumnType::kId:
        storage.reset(new storage::IdStorage(column_size));
        break;
      case ColumnType::kString:
        storage.reset(new storage::StringStorage(
            table->string_pool(),
            static_cast<const StringPool::Id*>(col.storage_base().data()),
            col.storage_base().non_null_size()));
        break;
      case ColumnType::kInt64:
      case ColumnType::kUint32:
      case ColumnType::kInt32:
      case ColumnType::kDouble:
        storage.reset(new storage::NumericStorage(
            col.storage_base().data(), col.storage_base().non_null_size(),
            col.col_type(), col.IsSorted()));
    }
    s_col.storage = storage.get();

    // Create cDBv2 overlays based on col.overlay()
    overlays::SelectorOverlay selector_overlay(
        col.overlay().row_map().GetIfBitVector());
    if (col.overlay().size() != column_size &&
        col.overlay().row_map().IsBitVector())
      s_col.overlays.emplace_back(&selector_overlay);

    overlays::ArrangementOverlay arrangement_overlay(
        col.overlay().row_map().GetIfIndexVector());
    if (col.overlay().row_map().IsIndexVector())
      s_col.overlays.emplace_back(&arrangement_overlay);

    // Add nullability
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
