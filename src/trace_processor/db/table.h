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

#ifndef SRC_TRACE_PROCESSOR_DB_TABLE_H_
#define SRC_TRACE_PROCESSOR_DB_TABLE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "src/trace_processor/containers/row_map.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/db/column.h"
#include "src/trace_processor/db/column/data_layer.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/db/column_storage_overlay.h"
#include "src/trace_processor/db/query_executor.h"

namespace perfetto::trace_processor {

// Represents a table of data with named, strongly typed columns.
class Table {
 public:
  // Iterator over the rows of the table.
  class Iterator {
   public:
    explicit Iterator(const Table* table) : table_(table) {
      its_.reserve(table->overlays().size());
      for (const auto& rm : table->overlays()) {
        its_.emplace_back(rm.IterateRows());
      }
    }

    // Creates an iterator which iterates over |table| by first creating
    // overlays by Applying |apply| to the existing overlays and using the
    // indices there for iteration.
    explicit Iterator(const Table* table, RowMap apply) : table_(table) {
      overlays_.reserve(table->overlays().size());
      its_.reserve(table->overlays().size());
      for (const auto& rm : table->overlays()) {
        overlays_.emplace_back(rm.SelectRows(apply));
        its_.emplace_back(overlays_.back().IterateRows());
      }
    }

    Iterator(Iterator&&) noexcept = default;
    Iterator& operator=(Iterator&&) = default;

    Iterator(const Iterator&) = delete;
    Iterator& operator=(const Iterator&) = delete;

    // Advances the iterator to the next row of the table.
    Iterator& operator++() {
      for (auto& it : its_) {
        it.Next();
      }
      return *this;
    }

    // Returns whether the row the iterator is pointing at is valid.
    explicit operator bool() const { return its_[0]; }

    // Returns the value at the current row for column |col_idx|.
    SqlValue Get(uint32_t col_idx) const {
      const auto& col = table_->columns_[col_idx];
      return col.GetAtIdx(its_[col.overlay_index()].index());
    }

    // Returns the storage index for the current row for column |col_idx|.
    uint32_t StorageIndexForColumn(uint32_t col_idx) const {
      const auto& col = table_->columns_[col_idx];
      return its_[col.overlay_index()].index();
    }

    // Returns the storage index for the last overlay.
    uint32_t StorageIndexForLastOverlay() const { return its_.back().index(); }

   private:
    const Table* table_ = nullptr;
    std::vector<ColumnStorageOverlay> overlays_;
    std::vector<ColumnStorageOverlay::Iterator> its_;
  };

  // Helper class storing the schema of the table. This allows decisions to be
  // made about operations on the table without materializing the table - this
  // may be expensive for dynamically computed tables.
  //
  // Subclasses of Table usually provide a method (named Schema()) to statically
  // generate an instance of this class.
  struct Schema {
    struct Column {
      std::string name;
      SqlValue::Type type;
      bool is_id;
      bool is_sorted;
      bool is_hidden;
      bool is_set_id;
    };
    std::vector<Column> columns;
  };

  static bool kUseFilterV2;
  static bool kUseSortV2;

  virtual ~Table();

  // We explicitly define the move constructor here because we need to update
  // the Table pointer in each column in the table.
  Table(Table&& other) noexcept { *this = std::move(other); }
  Table& operator=(Table&& other) noexcept;

  // Return a chain corresponding to a given column.
  const column::DataLayerChain& ChainForColumn(uint32_t col_idx) const {
    return *chains_[col_idx];
  }

  // Filters and sorts the tables with the arguments specified, returning the
  // result as a RowMap.
  RowMap QueryToRowMap(
      const std::vector<Constraint>&,
      const std::vector<Order>&,
      RowMap::OptimizeFor = RowMap::OptimizeFor::kMemory) const;

  // Applies the RowMap |rm| onto this table and returns an iterator over the
  // resulting rows.
  Iterator ApplyAndIterateRows(RowMap rm) const {
    return Iterator(this, std::move(rm));
  }

  // Sorts the table using the specified order by constraints.
  Table Sort(const std::vector<Order>&) const;

  // Returns an iterator over the rows in this table.
  Iterator IterateRows() const { return Iterator(this); }

  // Creates a copy of this table.
  Table Copy() const;

  uint32_t row_count() const { return row_count_; }
  StringPool* string_pool() const { return string_pool_; }
  const std::vector<ColumnLegacy>& columns() const { return columns_; }
  const std::vector<RefPtr<column::DataLayer>>& storage_layers() const {
    return storage_layers_;
  }
  const std::vector<RefPtr<column::DataLayer>>& null_layers() const {
    return null_layers_;
  }

 protected:
  Table(StringPool*,
        uint32_t row_count,
        std::vector<ColumnLegacy>,
        std::vector<ColumnStorageOverlay>);

  void CopyLastInsertFrom(const std::vector<ColumnStorageOverlay>& overlays) {
    PERFETTO_DCHECK(overlays.size() <= overlays_.size());

    // Add the last inserted row in each of the parent row maps to the
    // corresponding row map in the child.
    for (uint32_t i = 0; i < overlays.size(); ++i) {
      const ColumnStorageOverlay& other = overlays[i];
      overlays_[i].Insert(other.Get(other.size() - 1));
    }
  }

  void IncrementRowCountAndAddToLastOverlay() {
    // Also add the index of the new row to the identity row map and increment
    // the size.
    overlays_.back().Insert(row_count_++);
  }

  void OnConstructionCompleted(
      std::vector<RefPtr<column::DataLayer>> storage_layers,
      std::vector<RefPtr<column::DataLayer>> null_layers,
      std::vector<RefPtr<column::DataLayer>> overlay_layers);

  ColumnLegacy* GetColumn(uint32_t index) { return &columns_[index]; }

  const std::vector<ColumnStorageOverlay>& overlays() const {
    return overlays_;
  }

 private:
  friend class ColumnLegacy;

  PERFETTO_ALWAYS_INLINE RowMap FilterToRowMap(
      const std::vector<Constraint>& cs,
      RowMap::OptimizeFor optimize_for = RowMap::OptimizeFor::kMemory) const {
    if (cs.empty()) {
      return {0, row_count_, optimize_for};
    }

    if (kUseFilterV2) {
      if (optimize_for == RowMap::OptimizeFor::kMemory) {
        return QueryExecutor::FilterLegacy(this, cs);
      }
      return RowMap(QueryExecutor::FilterLegacy(this, cs).TakeAsIndexVector());
    }
    RowMap rm(0, row_count_, optimize_for);
    for (const Constraint& c : cs) {
      columns_[c.col_idx].FilterInto(c.op, c.value, &rm);
    }
    return rm;
  }

  Table CopyExceptOverlays() const;

  StringPool* string_pool_ = nullptr;
  uint32_t row_count_ = 0;
  std::vector<ColumnStorageOverlay> overlays_;
  std::vector<ColumnLegacy> columns_;

  std::vector<RefPtr<column::DataLayer>> storage_layers_;
  std::vector<RefPtr<column::DataLayer>> null_layers_;
  std::vector<RefPtr<column::DataLayer>> overlay_layers_;
  std::vector<std::unique_ptr<column::DataLayerChain>> chains_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_DB_TABLE_H_
