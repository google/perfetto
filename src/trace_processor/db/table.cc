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

#include "src/trace_processor/db/table.h"

namespace perfetto {
namespace trace_processor {

Table::Table() = default;
Table::~Table() = default;

Table::Table(StringPool* pool) : string_pool_(pool) {}

Table& Table::operator=(Table&& other) noexcept {
  row_count_ = other.row_count_;
  string_pool_ = other.string_pool_;

  overlays_ = std::move(other.overlays_);
  columns_ = std::move(other.columns_);
  for (Column& col : columns_) {
    col.table_ = this;
  }
  return *this;
}

Table Table::Copy() const {
  Table table = CopyExceptOverlays();
  for (const ColumnStorageOverlay& overlay : overlays_) {
    table.overlays_.emplace_back(overlay.Copy());
  }
  return table;
}

Table Table::CopyExceptOverlays() const {
  Table table(string_pool_);
  table.row_count_ = row_count_;
  for (const Column& col : columns_) {
    table.columns_.emplace_back(col, &table, col.index_in_table(),
                                col.overlay_index());
  }
  return table;
}

Table Table::Sort(const std::vector<Order>& od) const {
  if (od.empty())
    return Copy();

  // Return a copy if there is a single constraint to sort the table
  // by a column which is already sorted.
  const auto& first_col = GetColumn(od.front().col_idx);
  if (od.size() == 1 && first_col.IsSorted() && !od.front().desc)
    return Copy();

  // Build an index vector with all the indices for the first |size_| rows.
  std::vector<uint32_t> idx(row_count_);

  if (od.size() == 1 && first_col.IsSorted()) {
    // We special case a single constraint in descending order as this
    // happens any time the |max| function is used in SQLite. We can be
    // more efficient as this column is already sorted so we simply need
    // to reverse the order of this column.
    PERFETTO_DCHECK(od.front().desc);
    std::iota(idx.rbegin(), idx.rend(), 0);
  } else {
    // As our data is columnar, it's always more efficient to sort one column
    // at a time rather than try and sort lexiographically all at once.
    // To preserve correctness, we need to stably sort the index vector once
    // for each order by in *reverse* order. Reverse order is important as it
    // preserves the lexiographical property.
    //
    // For example, suppose we have the following:
    // Table {
    //   Column x;
    //   Column y
    //   Column z;
    // }
    //
    // Then, to sort "y asc, x desc", we could do one of two things:
    //  1) sort the index vector all at once and on each index, we compare
    //     y then z. This is slow as the data is columnar and we need to
    //     repeatedly branch inside each column.
    //  2) we can stably sort first on x desc and then sort on y asc. This will
    //     first put all the x in the correct order such that when we sort on
    //     y asc, we will have the correct order of x where y is the same (since
    //     the sort is stable).
    //
    // TODO(lalitm): it is possible that we could sort the last constraint (i.e.
    // the first constraint in the below loop) in a non-stable way. However,
    // this is more subtle than it appears as we would then need special
    // handling where there are order bys on a column which is already sorted
    // (e.g. ts, id). Investigate whether the performance gains from this are
    // worthwhile. This also needs changes to the constraint modification logic
    // in DbSqliteTable which currently eliminates constraints on sorted
    // columns.
    std::iota(idx.begin(), idx.end(), 0);
    for (auto it = od.rbegin(); it != od.rend(); ++it) {
      columns_[it->col_idx].StableSort(it->desc, &idx);
    }
  }

  // Return a copy of this table with the RowMaps using the computed ordered
  // RowMap.
  Table table = CopyExceptOverlays();
  RowMap rm(std::move(idx));
  for (const ColumnStorageOverlay& overlay : overlays_) {
    table.overlays_.emplace_back(overlay.SelectRows(rm));
    PERFETTO_DCHECK(table.overlays_.back().size() == table.row_count());
  }

  // Remove the sorted and row set flags from all the columns.
  for (auto& col : table.columns_) {
    col.flags_ &= ~Column::Flag::kSorted;
    col.flags_ &= ~Column::Flag::kSetId;
  }

  // For the first order by, make the column flag itself as sorted but
  // only if the sort was in ascending order.
  if (!od.front().desc) {
    table.columns_[od.front().col_idx].flags_ |= Column::Flag::kSorted;
  }

  return table;
}

}  // namespace trace_processor
}  // namespace perfetto
