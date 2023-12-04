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
#include <optional>

#include "perfetto/base/logging.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/row_map.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/db/column_storage.h"
#include "src/trace_processor/db/column_storage_overlay.h"
#include "src/trace_processor/db/compare.h"
#include "src/trace_processor/db/storage/types.h"
#include "src/trace_processor/db/typed_column_internal.h"

namespace perfetto {
namespace trace_processor {

// Helper class for converting a type to a ColumnType.
template <typename T>
struct ColumnTypeHelper;
template <>
struct ColumnTypeHelper<int32_t> {
  static constexpr ColumnType ToColumnType() { return ColumnType::kInt32; }
};
template <>
struct ColumnTypeHelper<uint32_t> {
  static constexpr ColumnType ToColumnType() { return ColumnType::kUint32; }
};
template <>
struct ColumnTypeHelper<int64_t> {
  static constexpr ColumnType ToColumnType() { return ColumnType::kInt64; }
};
template <>
struct ColumnTypeHelper<double> {
  static constexpr ColumnType ToColumnType() { return ColumnType::kDouble; }
};
template <>
struct ColumnTypeHelper<StringPool::Id> {
  static constexpr ColumnType ToColumnType() { return ColumnType::kString; }
};
template <typename T>
struct ColumnTypeHelper<std::optional<T>> : public ColumnTypeHelper<T> {};

class Table;

// Represents a named, strongly typed list of data.
class Column {
 public:
  // Flags which indicate properties of the data in the column. These features
  // are used to speed up column methods like filtering/sorting.
  enum Flag : uint32_t {
    // Indicates that this column has no special properties.
    kNoFlag = 0,

    // Indicates the data in the column is sorted. This can be used to speed
    // up filtering and skip sorting.
    kSorted = 1 << 0,

    // Indicates the data in the column is non-null. That is, the NullableVector
    // passed in will never have any null entries. This is only used for
    // numeric columns (string columns and id columns both have special
    // handling which ignores this flag).
    //
    // This is used to speed up filters as we can safely index NullableVector
    // directly if this flag is set.
    kNonNull = 1 << 1,

    // Indicates that the data in the column is "hidden". This can by used to
    // hint to users of Table and Column that this column should not be
    // displayed to the user as it is part of the internal implementation
    // details of the table.
    kHidden = 1 << 2,

    // Indicates that the data in this column is stored densely. This
    // allows for fast Set calls to change the data in the column.
    //
    // This flag is only meaningful for nullable columns has no effect for
    // non-null columns.
    kDense = 1 << 3,

    // Indicates that the sorted numeric data in the column is laid out such
    // that at row i, we will always have col[i] <= i and the first element, j
    // of each group happens at the index j.
    //
    // This is a common pattern in trace processor and columns with this
    // property are suffixed with "set_id" hence the name of this flag.
    //
    // To make this clear, here are some valid and invalid uses of this flag.
    //
    // Valid:
    // []
    // [0]
    // [0, 1, 2]
    // [0, 0, 2]
    // [0, 0, 0, 3, 3, 5, 6, 6, 7]
    //
    // Invalid:
    // [1]
    // [0, 0, 1]
    // [0, 0, 2, 5]
    // [0, 0, 2, 1]
    //
    // If this flag is set, kSorted and kNonNull should be set. Moreover, this
    // flag can only be set when the type is ColumnType::kUint32; other types
    // are not supported.
    kSetId = 1 << 4,
  };

  // Iterator over a column which conforms to std iterator interface
  // to allow using std algorithms (e.g. upper_bound, lower_bound etc.).
  class Iterator {
   public:
    using iterator_category = std::random_access_iterator_tag;
    using value_type = SqlValue;
    using difference_type = uint32_t;
    using pointer = uint32_t*;
    using reference = uint32_t&;

    Iterator(const Column* col, uint32_t row) : col_(col), row_(row) {}

    Iterator(const Iterator&) = default;
    Iterator& operator=(const Iterator&) = default;

    bool operator==(const Iterator& other) const { return other.row_ == row_; }
    bool operator!=(const Iterator& other) const { return !(*this == other); }
    bool operator<(const Iterator& other) const { return row_ < other.row_; }
    bool operator>(const Iterator& other) const { return other < *this; }
    bool operator<=(const Iterator& other) const { return !(other < *this); }
    bool operator>=(const Iterator& other) const { return !(*this < other); }

    SqlValue operator*() const { return col_->Get(row_); }
    Iterator& operator++() {
      row_++;
      return *this;
    }
    Iterator& operator--() {
      row_--;
      return *this;
    }

    Iterator& operator+=(uint32_t diff) {
      row_ += diff;
      return *this;
    }
    uint32_t operator-(const Iterator& other) const {
      return row_ - other.row_;
    }

    uint32_t row() const { return row_; }

   private:
    const Column* col_ = nullptr;
    uint32_t row_ = 0;
  };

  // Flags specified for an id column.
  static constexpr uint32_t kIdFlags = Flag::kSorted | Flag::kNonNull;

  // Flags which should *not* be inherited implicitly when a column is
  // assocaited to another table.
  static constexpr uint32_t kNoCrossTableInheritFlags = Column::Flag::kSetId;

  template <typename T>
  Column(const char* name,
         ColumnStorage<T>* storage,
         /* Flag */ uint32_t flags,
         Table* table,
         uint32_t col_idx_in_table,
         uint32_t row_map_idx)
      : Column(name,
               ColumnTypeHelper<stored_type<T>>::ToColumnType(),
               flags,
               table,
               col_idx_in_table,
               row_map_idx,
               storage) {}

  // Create a Column backed by the same data as |column| but is associated to a
  // different table and, optionally, having a different name.
  Column(const Column& column,
         Table* table,
         uint32_t col_idx_in_table,
         uint32_t row_map_idx,
         const char* name = nullptr);

  // Columns are movable but not copyable.
  Column(Column&&) noexcept = default;
  Column& operator=(Column&&) = default;

  // Creates a Column which does not have any data backing it.
  static Column DummyColumn(const char* name,
                            Table* table,
                            uint32_t col_idx_in_table);

  // Creates a Column which returns the index as the value of the row.
  static Column IdColumn(Table* table,
                         uint32_t col_idx_in_table,
                         uint32_t row_map_idx,
                         const char* name = "id",
                         uint32_t flags = kIdFlags);

  // Gets the value of the Column at the given |row|.
  SqlValue Get(uint32_t row) const { return GetAtIdx(overlay().Get(row)); }

  // Returns the row containing the given value in the Column.
  std::optional<uint32_t> IndexOf(SqlValue value) const {
    switch (type_) {
      // TODO(lalitm): investigate whether we could make this more efficient
      // by first checking the type of the column and comparing explicitly
      // based on that type.
      case ColumnType::kInt32:
      case ColumnType::kUint32:
      case ColumnType::kInt64:
      case ColumnType::kDouble:
      case ColumnType::kString: {
        for (uint32_t i = 0; i < overlay().size(); i++) {
          if (compare::SqlValue(Get(i), value) == 0)
            return i;
        }
        return std::nullopt;
      }
      case ColumnType::kId: {
        if (value.type != SqlValue::Type::kLong)
          return std::nullopt;
        return overlay().RowOf(static_cast<uint32_t>(value.long_value));
      }
      case ColumnType::kDummy:
        PERFETTO_FATAL("IndexOf not allowed on dummy column");
    }
    PERFETTO_FATAL("For GCC");
  }

  // Sorts |idx| in ascending or descending order (determined by |desc|) based
  // on the contents of this column.
  void StableSort(bool desc, std::vector<uint32_t>* idx) const;

  // Updates the given RowMap by only keeping rows where this column meets the
  // given filter constraint.
  void FilterInto(FilterOp op, SqlValue value, RowMap* rm) const {
    if (IsId() && op == FilterOp::kEq) {
      // If this is an equality constraint on an id column, try and find the
      // single row with the id (if it exists).
      auto opt_idx = IndexOf(value);
      if (opt_idx) {
        rm->IntersectExact(*opt_idx);
      } else {
        rm->Clear();
      }
      return;
    }

    if (IsSetId() && op == FilterOp::kEq && value.type == SqlValue::kLong) {
      // If the column is sorted and the value has the same type as the column,
      // we should be able to just do a binary search to find the range of rows
      // instead of a full table scan.
      FilterIntoSetIdEq(value.AsLong(), rm);
      return;
    }

    if (IsSorted() && value.type == type()) {
      // If the column is sorted and the value has the same type as the column,
      // we should be able to just do a binary search to find the range of rows
      // instead of a full table scan.
      bool handled = FilterIntoSorted(op, value, rm);
      if (handled)
        return;
    }

    FilterIntoSlow(op, value, rm);
  }

  // Returns the minimum value in this column. Returns std::nullopt if this
  // column is empty.
  std::optional<SqlValue> Min() const {
    if (overlay().empty())
      return std::nullopt;

    if (IsSorted())
      return Get(0);

    Iterator b(this, 0);
    Iterator e(this, overlay().size());
    return *std::min_element(b, e, &compare::SqlValueComparator);
  }

  // Returns the minimum value in this column. Returns std::nullopt if this
  // column is empty.
  std::optional<SqlValue> Max() const {
    if (overlay().empty())
      return std::nullopt;

    if (IsSorted())
      return Get(overlay().size() - 1);

    Iterator b(this, 0);
    Iterator e(this, overlay().size());
    return *std::max_element(b, e, &compare::SqlValueComparator);
  }

  // Returns the backing RowMap for this Column.
  // This function is defined out of line because of a circular dependency
  // between |Table| and |Column|.
  const ColumnStorageOverlay& overlay() const;

  // Returns the name of the column.
  const char* name() const { return name_; }

  // Returns the type of this Column in terms of SqlValue::Type.
  SqlValue::Type type() const { return ToSqlValueType(type_); }

  // Returns the type of this Column in terms of ColumnType.
  ColumnType col_type() const { return type_; }

  // Test the type of this Column.
  template <typename T>
  bool IsColumnType() const {
    return ColumnTypeHelper<T>::ToColumnType() == type_;
  }

  // Returns true if this column is considered an id column.
  bool IsId() const { return type_ == ColumnType::kId; }

  // Returns true if this column is a nullable column.
  bool IsNullable() const { return IsNullable(flags_); }

  // Returns true if this column is a sorted column.
  bool IsSorted() const { return IsSorted(flags_); }

  // Returns true if this column is a dense column.
  bool IsDense() const { return IsDense(flags_); }

  // Returns true if this column is a set id column.
  // Public for testing.
  bool IsSetId() const { return IsSetId(flags_); }

  // Returns true if this column is a dummy column.
  // Public for testing.
  bool IsDummy() const { return type_ == ColumnType::kDummy; }

  // Returns the index of the RowMap in the containing table.
  uint32_t overlay_index() const { return overlay_index_; }

  // Returns the index of the current column in the containing table.
  uint32_t index_in_table() const { return index_in_table_; }

  // Returns a Constraint for each type of filter operation for this Column.
  Constraint eq_value(SqlValue value) const {
    return Constraint{index_in_table_, FilterOp::kEq, value};
  }
  Constraint gt_value(SqlValue value) const {
    return Constraint{index_in_table_, FilterOp::kGt, value};
  }
  Constraint lt_value(SqlValue value) const {
    return Constraint{index_in_table_, FilterOp::kLt, value};
  }
  Constraint ne_value(SqlValue value) const {
    return Constraint{index_in_table_, FilterOp::kNe, value};
  }
  Constraint ge_value(SqlValue value) const {
    return Constraint{index_in_table_, FilterOp::kGe, value};
  }
  Constraint le_value(SqlValue value) const {
    return Constraint{index_in_table_, FilterOp::kLe, value};
  }
  Constraint is_not_null() const {
    return Constraint{index_in_table_, FilterOp::kIsNotNull, SqlValue()};
  }
  Constraint is_null() const {
    return Constraint{index_in_table_, FilterOp::kIsNull, SqlValue()};
  }
  Constraint glob_value(SqlValue value) const {
    return Constraint{index_in_table_, FilterOp::kGlob, value};
  }

  Constraint regex_value(SqlValue value) const {
    return Constraint{index_in_table_, FilterOp::kRegex, value};
  }

  // Returns an Order for each Order type for this Column.
  Order ascending() const { return Order{index_in_table_, false}; }
  Order descending() const { return Order{index_in_table_, true}; }

  // Returns an iterator to the first entry in this column.
  Iterator begin() const { return Iterator(this, 0); }

  // Returns an iterator pointing beyond the last entry in this column.
  Iterator end() const { return Iterator(this, overlay().size()); }

  // Returns whether the given combination of flags when the column has the
  // given type is valid.
  template <typename T>
  static constexpr bool IsFlagsAndTypeValid(uint32_t flags) {
    return IsFlagsAndTypeValid(flags, ColumnTypeHelper<T>::ToColumnType());
  }

  template <typename T>
  using stored_type = typename tc_internal::TypeHandler<T>::stored_type;

  // Returns the backing sparse vector cast to contain data of type T.
  // Should only be called when |type_| == ToColumnType<T>().
  template <typename T>
  const ColumnStorage<stored_type<T>>& storage() const {
    PERFETTO_DCHECK(ColumnTypeHelper<T>::ToColumnType() == type_);
    PERFETTO_DCHECK(tc_internal::TypeHandler<T>::is_optional == IsNullable());
    return *static_cast<ColumnStorage<stored_type<T>>*>(storage_);
  }

  const ColumnStorageBase& storage_base() const { return *storage_; }

 protected:
  // Returns the backing sparse vector cast to contain data of type T.
  // Should only be called when |type_| == ToColumnType<T>().
  template <typename T>
  ColumnStorage<stored_type<T>>* mutable_storage() {
    PERFETTO_DCHECK(ColumnTypeHelper<T>::ToColumnType() == type_);
    PERFETTO_DCHECK(tc_internal::TypeHandler<T>::is_optional == IsNullable());
    return static_cast<ColumnStorage<stored_type<T>>*>(storage_);
  }

  // Returns true if this column is a hidden column.
  bool IsHidden() const { return (flags_ & Flag::kHidden) != 0; }

  const StringPool& string_pool() const { return *string_pool_; }

  // Returns the type of this Column in terms of SqlValue::Type.
  template <typename T>
  static SqlValue::Type ToSqlValueType() {
    return ToSqlValueType(ColumnTypeHelper<T>::ToColumnType());
  }

  static SqlValue ToSqlValue(double value) { return SqlValue::Double(value); }
  static SqlValue ToSqlValue(int32_t value) { return SqlValue::Long(value); }
  static SqlValue ToSqlValue(uint32_t value) { return SqlValue::Long(value); }
  static SqlValue ToSqlValue(int64_t value) { return SqlValue::Long(value); }
  static SqlValue ToSqlValue(NullTermStringView value) {
    return SqlValue::String(value.c_str());
  }

 private:
  friend class Table;
  friend class View;

  // Base constructor for this class which all other constructors call into.
  Column(const char* name,
         ColumnType type,
         uint32_t flags,
         Table* table,
         uint32_t col_idx_in_table,
         uint32_t overlay_index,
         ColumnStorageBase* nullable_vector);

  Column(const Column&) = delete;
  Column& operator=(const Column&) = delete;

  // Gets the value of the Column at the given |idx|.
  SqlValue GetAtIdx(uint32_t idx) const {
    switch (type_) {
      case ColumnType::kInt32:
        return GetAtIdxTyped<int32_t>(idx);
      case ColumnType::kUint32:
        return GetAtIdxTyped<uint32_t>(idx);
      case ColumnType::kInt64:
        return GetAtIdxTyped<int64_t>(idx);
      case ColumnType::kDouble:
        return GetAtIdxTyped<double>(idx);
      case ColumnType::kString: {
        auto str = GetStringPoolStringAtIdx(idx).c_str();
        return str == nullptr ? SqlValue() : SqlValue::String(str);
      }
      case ColumnType::kId:
        return SqlValue::Long(idx);
      case ColumnType::kDummy:
        PERFETTO_FATAL("GetAtIdx not allowed on dummy column");
    }
    PERFETTO_FATAL("For GCC");
  }

  template <typename T>
  SqlValue GetAtIdxTyped(uint32_t idx) const {
    if (IsNullable()) {
      auto opt_value = storage<std::optional<T>>().Get(idx);
      return opt_value ? ToSqlValue(*opt_value) : SqlValue();
    }
    return ToSqlValue(storage<T>().Get(idx));
  }

  // Optimized filter method for sorted columns.
  // Returns whether the constraint was handled by the method.
  bool FilterIntoSorted(FilterOp op, SqlValue value, RowMap* rm) const {
    PERFETTO_DCHECK(IsSorted());
    PERFETTO_DCHECK(value.type == type());

    Iterator b(this, 0);
    Iterator e(this, overlay().size());
    switch (op) {
      case FilterOp::kEq: {
        uint32_t beg = std::distance(
            b, std::lower_bound(b, e, value, &compare::SqlValueComparator));
        uint32_t end = std::distance(
            b, std::upper_bound(b, e, value, &compare::SqlValueComparator));
        rm->Intersect({beg, end});
        return true;
      }
      case FilterOp::kLe: {
        uint32_t end = std::distance(
            b, std::upper_bound(b, e, value, &compare::SqlValueComparator));
        rm->Intersect({0, end});
        return true;
      }
      case FilterOp::kLt: {
        uint32_t end = std::distance(
            b, std::lower_bound(b, e, value, &compare::SqlValueComparator));
        rm->Intersect({0, end});
        return true;
      }
      case FilterOp::kGe: {
        uint32_t beg = std::distance(
            b, std::lower_bound(b, e, value, &compare::SqlValueComparator));
        rm->Intersect({beg, overlay().size()});
        return true;
      }
      case FilterOp::kGt: {
        uint32_t beg = std::distance(
            b, std::upper_bound(b, e, value, &compare::SqlValueComparator));
        rm->Intersect({beg, overlay().size()});
        return true;
      }
      case FilterOp::kNe:
      case FilterOp::kIsNull:
      case FilterOp::kIsNotNull:
      case FilterOp::kGlob:
      case FilterOp::kRegex:
        break;
    }
    return false;
  }

  void FilterIntoSetIdEq(int64_t value, RowMap* rm) const {
    PERFETTO_DCHECK(!IsNullable());

    uint32_t filter_set_id = static_cast<uint32_t>(value);
    const auto& st = storage<uint32_t>();
    const ColumnStorageOverlay& ov = overlay();

    // If the set id is beyond the end of the column, there's no chance that
    // it exists.
    if (PERFETTO_UNLIKELY(filter_set_id >= st.size())) {
      rm->Clear();
      return;
    }

    uint32_t set_id = st.Get(ov.Get(filter_set_id));

    // If the set at that index does not equal the set id we're looking for, the
    // set id doesn't exist either.
    if (PERFETTO_UNLIKELY(set_id != filter_set_id)) {
      PERFETTO_DCHECK(set_id < filter_set_id);
      rm->Clear();
      return;
    }

    // Otherwise, find the end of the set and return the intersection for this.
    for (uint32_t i = set_id + 1; i < ov.size(); ++i) {
      if (st.Get(ov.Get(i)) != filter_set_id) {
        RowMap r(set_id, i);
        rm->Intersect(r);
        return;
      }
    }
    RowMap r(set_id, ov.size());
    rm->Intersect(r);
  }

  // Slow path filter method which will perform a full table scan.
  void FilterIntoSlow(FilterOp op, SqlValue value, RowMap* rm) const;

  // Slow path filter method for numerics which will perform a full table scan.
  template <typename T, bool is_nullable>
  void FilterIntoNumericSlow(FilterOp op, SqlValue value, RowMap* rm) const;

  // Slow path filter method for numerics with a comparator which will perform a
  // full table scan.
  template <typename T, bool is_nullable, typename Comparator = int(T)>
  void FilterIntoNumericWithComparatorSlow(FilterOp op,
                                           RowMap* rm,
                                           Comparator cmp) const;

  // Slow path filter method for strings which will perform a full table scan.
  void FilterIntoStringSlow(FilterOp op, SqlValue value, RowMap* rm) const;

  // Slow path filter method for ids which will perform a full table scan.
  void FilterIntoIdSlow(FilterOp op, SqlValue value, RowMap* rm) const;

  // Stable sorts this column storing the result in |out|.
  template <bool desc>
  void StableSort(std::vector<uint32_t>* out) const;

  // Stable sorts this column storing the result in |out|.
  // |T| and |is_nullable| should match the type and nullability of this column.
  template <bool desc, typename T, bool is_nullable>
  void StableSortNumeric(std::vector<uint32_t>* out) const;

  static constexpr bool IsDense(uint32_t flags) {
    return (flags & Flag::kDense) != 0;
  }
  static constexpr bool IsNullable(uint32_t flags) {
    return (flags & Flag::kNonNull) == 0;
  }
  static constexpr bool IsSetId(uint32_t flags) {
    return (flags & Flag::kSetId) != 0;
  }
  static constexpr bool IsSorted(uint32_t flags) {
    return (flags & Flag::kSorted) != 0;
  }

  static constexpr bool IsFlagsAndTypeValid(uint32_t flags, ColumnType type) {
    return (!IsDense(flags) || IsFlagsForDenseValid(flags)) &&
           (!IsSetId(flags) || IsFlagsAndTypeForSetIdValid(flags, type));
  }

  static constexpr bool IsFlagsForDenseValid(uint32_t flags) {
    // The dense flag should only be set when the column is nullable.
    return IsNullable(flags);
  }

  static constexpr bool IsFlagsAndTypeForSetIdValid(uint32_t flags,
                                                    ColumnType type) {
    // The sorted flag should always be set for set id columns.
    // The non-null flag should always be set for set id columns.
    // The column type should always be kUint32.
    return IsSorted(flags) && !IsNullable(flags) && type == ColumnType::kUint32;
  }

  static SqlValue::Type ToSqlValueType(ColumnType type) {
    switch (type) {
      case ColumnType::kInt32:
      case ColumnType::kUint32:
      case ColumnType::kInt64:
      case ColumnType::kId:
        return SqlValue::Type::kLong;
      case ColumnType::kDouble:
        return SqlValue::Type::kDouble;
      case ColumnType::kString:
        return SqlValue::Type::kString;
      case ColumnType::kDummy:
        PERFETTO_FATAL("ToSqlValueType not allowed on dummy column");
    }
    PERFETTO_FATAL("For GCC");
  }

  // Returns the string at the index |idx|.
  // Should only be called when |type_| == ColumnType::kString.
  NullTermStringView GetStringPoolStringAtIdx(uint32_t idx) const {
    PERFETTO_DCHECK(type_ == ColumnType::kString);
    return string_pool_->Get(storage<StringPool::Id>().Get(idx));
  }

  // type_ is used to cast nullable_vector_ to the correct type.
  ColumnType type_ = ColumnType::kInt64;
  ColumnStorageBase* storage_ = nullptr;

  const char* name_ = nullptr;
  uint32_t flags_ = Flag::kNoFlag;
  const Table* table_ = nullptr;
  uint32_t index_in_table_ = 0;
  uint32_t overlay_index_ = 0;
  const StringPool* string_pool_ = nullptr;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DB_COLUMN_H_
