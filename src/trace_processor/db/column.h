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

#ifndef SRC_TRACE_PROCESSOR_DB_COLUMN_H_
#define SRC_TRACE_PROCESSOR_DB_COLUMN_H_

#include <stdint.h>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/optional.h"
#include "src/trace_processor/db/row_map.h"
#include "src/trace_processor/db/sparse_vector.h"

namespace perfetto {
namespace trace_processor {

// Represents the possible filter operations on a column.
enum FilterOp {
  kEq,
  kGt,
  kLt,
};

// Represents a constraint on a column.
struct Constraint {
  uint32_t col_idx;
  FilterOp op;
  int64_t value;
};

// Represents an order by operation on a column.
struct Order {
  uint32_t col_idx;
  bool desc;
};

// Represents a column which is to be joined on.
struct JoinKey {
  uint32_t col_idx;
};

class Table;

// Represents a named, strongly typed list of data.
class Column {
 public:
  // Create an nullable int64 Column.
  // Note: |name| must be a long lived string.
  Column(const char* name,
         const SparseVector<int64_t>* storage,
         Table* table,
         uint32_t col_idx,
         uint32_t row_map_idx)
      : Column(name, ColumnType::kInt64, table, col_idx, row_map_idx) {
    data_.int64_sv = storage;
  }

  // Create a Column has the same name and is backed by the same data as
  // |column| but is associated to a different table.
  Column(const Column& column,
         Table* table,
         uint32_t col_idx,
         uint32_t row_map_idx)
      : Column(column.name_, column.type_, table, col_idx, row_map_idx) {
    data_ = column.data_;
  }

  Column(Column&&) noexcept = default;
  Column& operator=(Column&&) = default;

  // Creates a Column which returns the index as the value of the row.
  static Column IdColumn(Table* table, uint32_t col_idx, uint32_t row_map_idx) {
    return Column("id", ColumnType::kId, table, col_idx, row_map_idx);
  }

  // Gets the value of the Column at the given |row|
  base::Optional<int64_t> Get(uint32_t row) const {
    auto opt_idx = row_map().Get(row);
    switch (type_) {
      case ColumnType::kInt64:
        return data_.int64_sv->Get(opt_idx);
      case ColumnType::kId:
        return opt_idx;
    }
    PERFETTO_FATAL("For GCC");
  }

  // Returns the row containing the given value in the Column.
  base::Optional<uint32_t> IndexOf(int64_t value) const {
    switch (type_) {
      case ColumnType::kInt64:
        for (uint32_t i = 0; i < row_map().size(); i++) {
          if (Get(i) == value)
            return i;
        }
        return base::nullopt;
      case ColumnType::kId:
        return row_map().IndexOf(static_cast<uint32_t>(value));
    }
    PERFETTO_FATAL("For GCC");
  }

  // Updates the given RowMap by only keeping rows where this column meets the
  // given filter constraint.
  void FilterInto(FilterOp, int64_t value, RowMap*) const;

  // Returns a Constraint for each type of filter operation for this Column.
  Constraint eq(int64_t value) const {
    return Constraint{col_idx_, FilterOp::kEq, value};
  }
  Constraint gt(int64_t value) const {
    return Constraint{col_idx_, FilterOp::kGt, value};
  }
  Constraint lt(int64_t value) const {
    return Constraint{col_idx_, FilterOp::kLt, value};
  }

  // Returns an Order for each Order type for this Column.
  Order ascending() const { return Order{col_idx_, false}; }
  Order descending() const { return Order{col_idx_, true}; }

  // Returns the JoinKey for this Column.
  JoinKey join_key() const { return JoinKey{col_idx_}; }

  const RowMap& row_map() const;
  const char* name() const { return name_; }

 private:
  friend class Table;

  enum ColumnType {
    // Standard primitive types.
    kInt64,

    // Types generated on the fly.
    kId,
  };

  Column(const char* name,
         ColumnType type,
         Table* table,
         uint32_t col_idx,
         uint32_t row_map_idx)
      : name_(name),
        table_(table),
        col_idx_(col_idx),
        row_map_idx_(row_map_idx),
        type_(type) {}

  Column(const Column&) = delete;
  Column& operator=(const Column&) = delete;

  const char* name_ = nullptr;
  Table* table_ = nullptr;
  uint32_t col_idx_ = 0;
  uint32_t row_map_idx_ = 0;

  ColumnType type_ = ColumnType::kInt64;
  union {
    // Valid when |type_| == ColumnType::kInt64.
    const SparseVector<int64_t>* int64_sv = nullptr;
  } data_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DB_COLUMN_H_
