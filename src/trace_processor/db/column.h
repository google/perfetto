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
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/db/row_map.h"
#include "src/trace_processor/db/sparse_vector.h"
#include "src/trace_processor/string_pool.h"

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
  SqlValue value;
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
  // Create a nullable uint32 Column.
  // Note: |name| must be a long lived string.
  Column(const char* name,
         const SparseVector<uint32_t>* storage,
         Table* table,
         uint32_t col_idx,
         uint32_t row_map_idx)
      : Column(name, ColumnType::kUint32, table, col_idx, row_map_idx) {
    data_.uint32_sv = storage;
  }

  // Create a nullable int64 Column.
  // Note: |name| must be a long lived string.
  Column(const char* name,
         const SparseVector<int64_t>* storage,
         Table* table,
         uint32_t col_idx,
         uint32_t row_map_idx)
      : Column(name, ColumnType::kInt64, table, col_idx, row_map_idx) {
    data_.int64_sv = storage;
  }

  // Create an nullable string Column.
  // Note: |name| must be a long lived string.
  // TODO(lalitm): investigate changing this to a std::deque instead as
  // StringIds already have the concept of nullability in them.
  Column(const char* name,
         const SparseVector<StringPool::Id>* storage,
         Table* table,
         uint32_t col_idx,
         uint32_t row_map_idx)
      : Column(name, ColumnType::kString, table, col_idx, row_map_idx) {
    data_.string_sv = storage;
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
  SqlValue Get(uint32_t row) const {
    auto idx = row_map().Get(row);
    switch (type_) {
      case ColumnType::kUint32: {
        auto opt_value = data_.uint32_sv->Get(idx);
        return opt_value ? SqlValue::Long(*opt_value) : SqlValue();
      }
      case ColumnType::kInt64: {
        auto opt_value = data_.int64_sv->Get(idx);
        return opt_value ? SqlValue::Long(*opt_value) : SqlValue();
      }
      case ColumnType::kString: {
        auto opt_id = data_.string_sv->Get(idx);
        // We DCHECK here because although we are using SparseVector, the null
        // info is handled by the StringPool rather than by the SparseVector.
        // The value returned by the SparseVector should always be non-null.
        // TODO(lalitm): investigate removing this check if/when we support
        // std::deque<StringId>.
        PERFETTO_DCHECK(opt_id.has_value());
        auto str = string_pool_->Get(*opt_id).c_str();
        return str == nullptr ? SqlValue() : SqlValue::String(str);
      }
      case ColumnType::kId:
        return SqlValue::Long(idx);
    }
    PERFETTO_FATAL("For GCC");
  }

  // Returns the row containing the given value in the Column.
  base::Optional<uint32_t> IndexOf(SqlValue value) const {
    switch (type_) {
      // TODO(lalitm): investigate whether we could make this more efficient
      // by first checking the type of the column and comparing explicitly
      // based on that type.
      case ColumnType::kUint32:
      case ColumnType::kInt64:
      case ColumnType::kString: {
        for (uint32_t i = 0; i < row_map().size(); i++) {
          if (Get(i) == value)
            return i;
        }
        return base::nullopt;
      }
      case ColumnType::kId: {
        if (value.type != SqlValue::Type::kLong)
          return base::nullopt;
        return row_map().IndexOf(static_cast<uint32_t>(value.long_value));
      }
    }
    PERFETTO_FATAL("For GCC");
  }

  // Updates the given RowMap by only keeping rows where this column meets the
  // given filter constraint.
  void FilterInto(FilterOp, SqlValue value, RowMap*) const;

  // Returns a Constraint for each type of filter operation for this Column.
  Constraint eq(SqlValue value) const {
    return Constraint{col_idx_, FilterOp::kEq, value};
  }
  Constraint gt(SqlValue value) const {
    return Constraint{col_idx_, FilterOp::kGt, value};
  }
  Constraint lt(SqlValue value) const {
    return Constraint{col_idx_, FilterOp::kLt, value};
  }

  // Returns an Order for each Order type for this Column.
  Order ascending() const { return Order{col_idx_, false}; }
  Order descending() const { return Order{col_idx_, true}; }

  // Returns the JoinKey for this Column.
  JoinKey join_key() const { return JoinKey{col_idx_}; }

  const RowMap& row_map() const;
  const char* name() const { return name_; }

  SqlValue::Type type() const {
    switch (type_) {
      case ColumnType::kUint32:
      case ColumnType::kInt64:
      case ColumnType::kId:
        return SqlValue::Type::kLong;
      case ColumnType::kString:
        return SqlValue::Type::kString;
    }
    PERFETTO_FATAL("For GCC");
  }

 private:
  friend class Table;

  enum ColumnType {
    // Standard primitive types.
    kUint32,
    kInt64,
    kString,

    // Types generated on the fly.
    kId,
  };

  Column(const char* name,
         ColumnType type,
         Table* table,
         uint32_t col_idx,
         uint32_t row_map_idx);

  Column(const Column&) = delete;
  Column& operator=(const Column&) = delete;

  const char* name_ = nullptr;
  const Table* table_ = nullptr;
  const StringPool* string_pool_ = nullptr;
  uint32_t col_idx_ = 0;
  uint32_t row_map_idx_ = 0;

  ColumnType type_ = ColumnType::kInt64;
  union {
    // Valid when |type_| == ColumnType::kUint32.
    const SparseVector<uint32_t>* uint32_sv;

    // Valid when |type_| == ColumnType::kInt64.
    const SparseVector<int64_t>* int64_sv = nullptr;

    // Valid when |type_| == ColumnType::kString.
    const SparseVector<StringPool::Id>* string_sv;
  } data_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DB_COLUMN_H_
