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

#include <stdint.h>

#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/containers/row_map.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/db/column.h"
#include "src/trace_processor/db/column_storage_overlay.h"
#include "src/trace_processor/db/query_executor.h"
#include "src/trace_processor/db/typed_column.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto {
namespace trace_processor {

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

    Iterator(Iterator&&) noexcept = default;
    Iterator& operator=(Iterator&&) = default;

    Iterator(const Iterator&) = delete;
    Iterator& operator=(const Iterator&) = delete;

    // Advances the iterator to the next row of the table.
    void Next() {
      for (auto& it : its_) {
        it.Next();
      }
    }

    // Returns whether the row the iterator is pointing at is valid.
    explicit operator bool() const { return its_[0]; }

    // Returns the value at the current row for column |col_idx|.
    SqlValue Get(uint32_t col_idx) const {
      const auto& col = table_->columns_[col_idx];
      return col.GetAtIdx(its_[col.overlay_index()].index());
    }

   private:
    const Table* table_ = nullptr;
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

  Table();
  virtual ~Table();

  // We explicitly define the move constructor here because we need to update
  // the Table pointer in each column in the table.
  Table(Table&& other) noexcept { *this = std::move(other); }
  Table& operator=(Table&& other) noexcept;

  // Filters the Table using the specified filter constraints.
  Table Filter(
      const std::vector<Constraint>& cs,
      RowMap::OptimizeFor optimize_for = RowMap::OptimizeFor::kMemory) const {
    if (cs.empty())
      return Copy();
    return Apply(FilterToRowMap(cs, optimize_for));
  }

  // Filters the Table using the specified filter constraints optionally
  // specifying what the returned RowMap should optimize for.
  // Returns a RowMap which, if applied to the table, would contain the rows
  // post filter.
  RowMap FilterToRowMap(
      const std::vector<Constraint>& cs,
      RowMap::OptimizeFor optimize_for = RowMap::OptimizeFor::kMemory) const {
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

  // Applies the given RowMap to the current table by picking out the rows
  // specified in the RowMap to be present in the output table.
  // Note: the RowMap should not reorder this table; this is guaranteed if the
  // passed RowMap is generated using |FilterToRowMap|.
  Table Apply(RowMap rm) const {
    Table table = CopyExceptOverlays();
    table.row_count_ = rm.size();
    table.overlays_.reserve(overlays_.size());
    for (const ColumnStorageOverlay& map : overlays_) {
      table.overlays_.emplace_back(map.SelectRows(rm));
      PERFETTO_DCHECK(table.overlays_.back().size() == table.row_count());
    }
    // Pretty much any application of a RowMap will break the requirements on
    // kSetId so remove it.
    for (auto& col : table.columns_) {
      col.flags_ &= ~Column::Flag::kSetId;
    }
    return table;
  }

  // Sorts the Table using the specified order by constraints.
  Table Sort(const std::vector<Order>& od) const;

  // Returns the column at index |idx| in the Table.
  const Column& GetColumn(uint32_t idx) const { return columns_[idx]; }

  // Returns the column index with the given name or std::nullopt otherwise.
  std::optional<uint32_t> GetColumnIndexByName(const char* name) const {
    auto it = std::find_if(
        columns_.begin(), columns_.end(),
        [name](const Column& col) { return strcmp(col.name(), name) == 0; });
    if (it == columns_.end())
      return std::nullopt;
    return static_cast<uint32_t>(std::distance(columns_.begin(), it));
  }

  // Returns the column with the given name or nullptr otherwise.
  const Column* GetColumnByName(const char* name) const {
    std::optional<uint32_t> opt_idx = GetColumnIndexByName(name);
    if (!opt_idx)
      return nullptr;
    return &columns_[*opt_idx];
  }

  template <typename T>
  const TypedColumn<T>& GetTypedColumnByName(const char* name) const {
    return *TypedColumn<T>::FromColumn(GetColumnByName(name));
  }

  template <typename T>
  const IdColumn<T>& GetIdColumnByName(const char* name) const {
    return *IdColumn<T>::FromColumn(GetColumnByName(name));
  }

  // Returns the number of columns in the Table.
  uint32_t GetColumnCount() const {
    return static_cast<uint32_t>(columns_.size());
  }

  // Returns an iterator into the Table.
  Iterator IterateRows() const { return Iterator(this); }

  // Creates a copy of this table.
  Table Copy() const;

  // Computes the schema of this table and returns it.
  Schema ComputeSchema() const {
    Schema schema;
    schema.columns.reserve(columns_.size());
    for (const auto& col : columns_) {
      schema.columns.emplace_back(
          Schema::Column{col.name(), col.type(), col.IsId(), col.IsSorted(),
                         col.IsHidden(), col.IsSetId()});
    }
    return schema;
  }

  uint32_t row_count() const { return row_count_; }
  StringPool* string_pool() const { return string_pool_; }
  const std::vector<ColumnStorageOverlay>& overlays() const {
    return overlays_;
  }
  const std::vector<Column>& columns() const { return columns_; }

 protected:
  explicit Table(StringPool* pool);

  std::vector<ColumnStorageOverlay> CopyOverlays() const {
    std::vector<ColumnStorageOverlay> rm(overlays_.size());
    for (uint32_t i = 0; i < overlays_.size(); ++i) {
      rm[i] = overlays_[i].Copy();
    }
    return rm;
  }

  std::vector<ColumnStorageOverlay> overlays_;
  std::vector<Column> columns_;
  uint32_t row_count_ = 0;

  StringPool* string_pool_ = nullptr;

 private:
  friend class Column;
  friend class View;

  Table CopyExceptOverlays() const;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DB_TABLE_H_
