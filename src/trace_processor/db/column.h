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
enum class FilterOp {
  kEq,
  kNe,
  kGt,
  kLt,
  kGe,
  kLe,
  kIsNull,
  kIsNotNull,
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
  // Flags which indicate properties of the data in the column. These features
  // are used to speed up column methods like filtering/sorting.
  enum Flag : uint32_t {
    // Indicates that this column has no special properties.
    kNoFlag = 0,

    // Indiciates that the column is an "id" column. Specifically, this means
    // the backing data for this column has the property that data[i] = i;
    //
    // Note: generally, this flag should not be passed by users of this class.
    // Instead they should use the Column::IdColumn method to create an id
    // column.
    kId = 1 << 0,

    // Indicates the data in the column is sorted. This can be used to speed
    // up filtering and skip sorting.
    kSorted = 1 << 1,
  };

  template <typename T>
  Column(const char* name,
         SparseVector<T>* storage,
         /* Flag */ uint32_t flags,
         Table* table,
         uint32_t col_idx,
         uint32_t row_map_idx)
      : Column(name,
               ToColumnType<T>(),
               flags,
               table,
               col_idx,
               row_map_idx,
               storage) {}

  // Create a Column has the same name and is backed by the same data as
  // |column| but is associated to a different table.
  Column(const Column& column,
         Table* table,
         uint32_t col_idx,
         uint32_t row_map_idx);

  // Columns are movable but not copyable.
  Column(Column&&) noexcept = default;
  Column& operator=(Column&&) = default;

  // Creates a Column which returns the index as the value of the row.
  static Column IdColumn(Table* table, uint32_t col_idx, uint32_t row_map_idx);

  // Gets the value of the Column at the given |row|.
  SqlValue Get(uint32_t row) const {
    switch (type_) {
      case ColumnType::kInt32: {
        auto opt_value = GetTyped<int32_t>(row);
        return opt_value ? SqlValue::Long(*opt_value) : SqlValue();
      }
      case ColumnType::kUint32: {
        auto opt_value = GetTyped<uint32_t>(row);
        return opt_value ? SqlValue::Long(*opt_value) : SqlValue();
      }
      case ColumnType::kInt64: {
        auto opt_value = GetTyped<int64_t>(row);
        return opt_value ? SqlValue::Long(*opt_value) : SqlValue();
      }
      case ColumnType::kString: {
        auto str = GetStringPoolString(row).c_str();
        return str == nullptr ? SqlValue() : SqlValue::String(str);
      }
      case ColumnType::kId:
        return SqlValue::Long(row_map().Get(row));
    }
    PERFETTO_FATAL("For GCC");
  }

  // Returns the row containing the given value in the Column.
  base::Optional<uint32_t> IndexOf(SqlValue value) const {
    switch (type_) {
      // TODO(lalitm): investigate whether we could make this more efficient
      // by first checking the type of the column and comparing explicitly
      // based on that type.
      case ColumnType::kInt32:
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

  // Returns true if this column is considered an id column.
  bool IsId() const { return (flags_ & Flag::kId) != 0; }

  const RowMap& row_map() const;
  const char* name() const { return name_; }
  SqlValue::Type type() const {
    switch (type_) {
      case ColumnType::kInt32:
      case ColumnType::kUint32:
      case ColumnType::kInt64:
      case ColumnType::kId:
        return SqlValue::Type::kLong;
      case ColumnType::kString:
        return SqlValue::Type::kString;
    }
    PERFETTO_FATAL("For GCC");
  }

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
  Constraint ne(SqlValue value) const {
    return Constraint{col_idx_, FilterOp::kNe, value};
  }
  Constraint ge(SqlValue value) const {
    return Constraint{col_idx_, FilterOp::kGe, value};
  }
  Constraint le(SqlValue value) const {
    return Constraint{col_idx_, FilterOp::kLe, value};
  }
  Constraint is_not_null() const {
    return Constraint{col_idx_, FilterOp::kIsNotNull, SqlValue()};
  }
  Constraint is_null() const {
    return Constraint{col_idx_, FilterOp::kIsNull, SqlValue()};
  }

  // Returns an Order for each Order type for this Column.
  Order ascending() const { return Order{col_idx_, false}; }
  Order descending() const { return Order{col_idx_, true}; }

  // Returns the JoinKey for this Column.
  JoinKey join_key() const { return JoinKey{col_idx_}; }

 protected:
  enum class ColumnType {
    // Standard primitive types.
    kInt32,
    kUint32,
    kInt64,
    kString,

    // Types generated on the fly.
    kId,
  };

  template <typename T>
  base::Optional<T> GetTyped(uint32_t row) const {
    PERFETTO_DCHECK(ToColumnType<T>() == type_);
    auto idx = row_map().Get(row);
    return sparse_vector<T>().Get(idx);
  }

  template <typename T>
  void SetTyped(uint32_t row, T value) {
    PERFETTO_DCHECK(ToColumnType<T>() == type_);
    auto idx = row_map().Get(row);
    return mutable_sparse_vector<T>()->Set(idx, value);
  }

  NullTermStringView GetStringPoolString(uint32_t row) const {
    return string_pool_->Get(*GetTyped<StringPool::Id>(row));
  }

  template <typename T>
  const SparseVector<T>& sparse_vector() const {
    return *static_cast<const SparseVector<T>*>(sparse_vector_);
  }

  template <typename T>
  SparseVector<T>* mutable_sparse_vector() {
    return static_cast<SparseVector<T>*>(sparse_vector_);
  }

  // type_ is used to cast sparse_vector_ to the correct type.
  ColumnType type_ = ColumnType::kInt64;
  void* sparse_vector_ = nullptr;

 private:
  friend class Table;

  Column(const char* name,
         ColumnType type,
         uint32_t flags,
         Table* table,
         uint32_t col_idx,
         uint32_t row_map_idx,
         void* sparse_vector);

  Column(const Column&) = delete;
  Column& operator=(const Column&) = delete;

  template <typename T>
  static ColumnType ToColumnType() {
    if (std::is_same<T, uint32_t>::value) {
      return ColumnType::kUint32;
    } else if (std::is_same<T, int64_t>::value) {
      return ColumnType::kInt64;
    } else if (std::is_same<T, int32_t>::value) {
      return ColumnType::kInt32;
    } else if (std::is_same<T, StringPool::Id>::value) {
      return ColumnType::kString;
    } else {
      PERFETTO_FATAL("Unsupported type of column");
    }
  }

  const char* name_ = nullptr;
  uint32_t flags_ = Flag::kNoFlag;
  const Table* table_ = nullptr;
  uint32_t col_idx_ = 0;
  uint32_t row_map_idx_ = 0;
  const StringPool* string_pool_ = nullptr;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DB_COLUMN_H_
