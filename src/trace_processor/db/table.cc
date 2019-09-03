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

Table::Table(const StringPool* pool, const Table* parent) : string_pool_(pool) {
  if (!parent)
    return;

  // If this table has a parent, then copy over all the columns pointing to
  // empty RowMaps.
  for (uint32_t i = 0; i < parent->row_maps_.size(); ++i)
    row_maps_.emplace_back(BitVector());
  for (const Column& col : parent->columns_)
    columns_.emplace_back(col, this, columns_.size(), col.row_map_idx_);
}

Table& Table::operator=(const Table& other) {
  for (const RowMap& rm : other.row_maps_) {
    row_maps_.emplace_back(rm.Copy());
  }
  size_ = other.size_;
  for (const Column& col : other.columns_)
    columns_.emplace_back(col, this, columns_.size(), col.row_map_idx_);
  return *this;
}

Table& Table::operator=(Table&& other) noexcept {
  row_maps_ = std::move(other.row_maps_);
  columns_ = std::move(other.columns_);
  size_ = other.size_;
  for (Column& col : columns_) {
    col.table_ = this;
  }
  return *this;
}

Table Table::Filter(const std::vector<Constraint>& cs) const {
  // TODO(lalitm): we can add optimizations here depending on whether this is
  // an lvalue or rvalue.

  if (cs.empty())
    return *this;

  // Create a RowMap indexing all rows and filter this down to the rows which
  // meet all the constraints.
  RowMap rm(BitVector(size_, true));
  for (const Constraint& c : cs) {
    columns_[c.col_idx].FilterInto(c.op, c.value, &rm);
  }

  // Return a copy of this table with the RowMaps using the computed filter
  // RowMap and with the updated size.
  Table table(*this);
  for (RowMap& map : table.row_maps_) {
    map.SelectRows(rm);
  }
  table.size_ = rm.size();
  return table;
}

Table Table::Sort(const std::vector<Order>& od) const {
  // TODO(lalitm): we can add optimizations here depending on whether this is
  // an lvalue or rvalue.

  if (od.empty())
    return *this;

  // Build an index vector with all the indices for the first |size_| rows.
  std::vector<uint32_t> idx(size_);
  std::iota(idx.begin(), idx.end(), 0);

  // Sort the row indices according to the given order by constraints.
  std::sort(idx.begin(), idx.end(), [this, &od](uint32_t a, uint32_t b) {
    for (const Order& o : od) {
      const Column& col = columns_[o.col_idx];
      int cmp =
          col.Get(a) < col.Get(b) ? -1 : (col.Get(b) < col.Get(a) ? 1 : 0);
      if (cmp != 0)
        return o.desc ? cmp > 0 : cmp < 0;
    }
    return false;
  });

  // Return a copy of this table with the RowMaps using the computed ordered
  // RowMap.
  RowMap rm(std::move(idx));
  Table table(*this);
  for (RowMap& map : table.row_maps_) {
    map.SelectRows(rm);
    PERFETTO_DCHECK(map.size() == table.size());
  }
  return table;
}

Table Table::LookupJoin(JoinKey left, const Table& other, JoinKey right) {
  // The join table will have the same size and RowMaps as the left (this)
  // table because the left column is indexing the right table.
  Table table(string_pool_, nullptr);
  table.size_ = size_;
  for (const RowMap& rm : row_maps_) {
    table.row_maps_.emplace_back(rm.Copy());
  }

  for (const Column& col : columns_) {
    // We skip id columns as they are misleading on join tables.
    if (strcmp(col.name_, "id") == 0)
      continue;
    table.columns_.emplace_back(col, &table, table.columns_.size(),
                                col.row_map_idx_);
  }

  const Column& left_col = columns_[left.col_idx];
  const Column& right_col = other.columns_[right.col_idx];

  // For each index in the left column, retrieve the index of the row inside
  // the RowMap of the right column. By getting the index of the row rather
  // than the row number itself, we can call |Apply| on the other RowMaps
  // in the right table.
  std::vector<uint32_t> indices(size_);
  for (uint32_t i = 0; i < size_; ++i) {
    SqlValue val = left_col.Get(i);
    PERFETTO_CHECK(val.type != SqlValue::Type::kNull);
    indices[i] = right_col.IndexOf(val).value();
  }

  // Apply the computed RowMap to each of the right RowMaps, adding it to the
  // join table as we go.
  RowMap rm(std::move(indices));
  for (const RowMap& ot : other.row_maps_) {
    table.row_maps_.emplace_back(ot.Copy());
    table.row_maps_.back().SelectRows(rm);
  }

  uint32_t left_row_maps_size = static_cast<uint32_t>(row_maps_.size());
  for (const Column& col : other.columns_) {
    // We skip id columns as they are misleading on join tables.
    if (strcmp(col.name_, "id") == 0)
      continue;

    // Ensure that we offset the RowMap index by the number of RowMaps in the
    // left table.
    table.columns_.emplace_back(col, &table, table.columns_.size(),
                                col.row_map_idx_ + left_row_maps_size);
  }
  return table;
}

}  // namespace trace_processor
}  // namespace perfetto
