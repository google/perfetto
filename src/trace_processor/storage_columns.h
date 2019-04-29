/*
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

#ifndef SRC_TRACE_PROCESSOR_STORAGE_COLUMNS_H_
#define SRC_TRACE_PROCESSOR_STORAGE_COLUMNS_H_

#include <deque>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "src/trace_processor/filtered_row_index.h"
#include "src/trace_processor/sqlite_utils.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

// A column of data backed by data storage.
class StorageColumn {
 public:
  struct Bounds {
    uint32_t min_idx = 0;
    uint32_t max_idx = std::numeric_limits<uint32_t>::max();
    bool consumed = false;
  };
  using Predicate = std::function<bool(uint32_t)>;
  using Comparator = std::function<int(uint32_t, uint32_t)>;

  StorageColumn(std::string col_name, bool hidden);
  virtual ~StorageColumn();

  // Implements StorageCursor::ColumnReporter.
  virtual void ReportResult(sqlite3_context*, uint32_t row) const = 0;

  // Given a SQLite operator and value for the comparision, returns a
  // predicate which takes in a row index and returns whether the row should
  // be returned.
  virtual void Filter(int op, sqlite3_value*, FilteredRowIndex*) const = 0;

  // Given a order by constraint for this column, returns a comparator
  // function which compares data in this column at two indices.
  virtual Comparator Sort(const QueryConstraints::OrderBy& ob) const = 0;

  // Returns the type of this column.
  virtual Table::ColumnType GetType() const = 0;

  // Bounds a filter on this column between a minimum and maximum index.
  // Generally this is only possible if the column is sorted.
  virtual Bounds BoundFilter(int, sqlite3_value*) const { return Bounds{}; }

  // Returns whether this column is ordered.
  virtual bool HasOrdering() const { return false; }

  const std::string& name() const { return col_name_; }
  bool hidden() const { return hidden_; }

 private:
  std::string col_name_;
  bool hidden_ = false;
};

// The implementation of StorageColumn for Strings.
// The actual retrieval of the numerics from the data types is left to the
// Acessor trait (see below for definition).
template <typename Accessor /* <NullTermStringView> */>
class StringColumn final : public StorageColumn {
 public:
  StringColumn(std::string col_name, Accessor accessor, bool hidden = false)
      : StorageColumn(col_name, hidden), accessor_(accessor) {}

  void ReportResult(sqlite3_context* ctx, uint32_t row) const override {
    NullTermStringView str = accessor_.Get(row);
    if (str.c_str() == nullptr) {
      sqlite3_result_null(ctx);
    } else {
      sqlite3_result_text(ctx, str.c_str(), -1, sqlite_utils::kSqliteStatic);
    }
  }

  Bounds BoundFilter(int, sqlite3_value*) const override {
    Bounds bounds;
    bounds.max_idx = static_cast<uint32_t>(accessor_.Size());
    return bounds;
  }

  void Filter(int, sqlite3_value*, FilteredRowIndex*) const override {}

  Comparator Sort(const QueryConstraints::OrderBy& ob) const override {
    if (ob.desc) {
      return [this](uint32_t f, uint32_t s) {
        NullTermStringView a = accessor_.Get(f);
        NullTermStringView b = accessor_.Get(s);
        return sqlite_utils::CompareValuesDesc(a, b);
      };
    }
    return [this](uint32_t f, uint32_t s) {
      NullTermStringView a = accessor_.Get(f);
      NullTermStringView b = accessor_.Get(s);
      return sqlite_utils::CompareValuesAsc(a, b);
    };
  }

  Table::ColumnType GetType() const override {
    return Table::ColumnType::kString;
  }

  bool HasOrdering() const override { return accessor_.HasOrdering(); }

 private:
  Accessor accessor_;
};

// The implementation of StorageColumn for numeric data types.
// The actual retrieval of the numerics from the data types is left to the
// Acessor trait (see below for definition).
template <typename Accessor,
          typename sqlite_utils::is_numeric<typename Accessor::Type>* = nullptr>
class NumericColumn : public StorageColumn {
 public:
  // The type of the column. This is one of uint32_t, int32_t, uint64_t etc.
  using NumericType = typename Accessor::Type;

  NumericColumn(std::string col_name, bool hidden, Accessor accessor)
      : StorageColumn(col_name, hidden), accessor_(accessor) {}
  ~NumericColumn() override = default;

  void ReportResult(sqlite3_context* ctx, uint32_t row) const override {
    sqlite_utils::ReportSqliteResult(ctx, accessor_.Get(row));
  }

  Bounds BoundFilter(int op, sqlite3_value* sqlite_val) const override {
    Bounds bounds;
    bounds.max_idx = accessor_.Size();

    if (!accessor_.HasOrdering())
      return bounds;

    // Makes the below code much more readable.
    using namespace sqlite_utils;

    NumericType min = kTMin;
    NumericType max = kTMax;
    if (IsOpGe(op) || IsOpGt(op)) {
      min = FindGtBound<NumericType>(IsOpGe(op), sqlite_val);
    } else if (IsOpLe(op) || IsOpLt(op)) {
      max = FindLtBound<NumericType>(IsOpLe(op), sqlite_val);
    } else if (IsOpEq(op)) {
      auto val = FindEqBound<NumericType>(sqlite_val);
      min = val;
      max = val;
    }

    if (min <= kTMin && max >= kTMax)
      return bounds;

    bounds.min_idx = accessor_.LowerBoundIndex(min);
    bounds.max_idx = accessor_.UpperBoundIndex(max);
    bounds.consumed = true;

    return bounds;
  }

  void Filter(int op,
              sqlite3_value* value,
              FilteredRowIndex* index) const override {
    auto type = sqlite3_value_type(value);

    bool same_type = (kIsIntegralType && type == SQLITE_INTEGER) ||
                     (kIsRealType && type == SQLITE_FLOAT);
    if (sqlite_utils::IsOpEq(op) && same_type &&
        accessor_.CanFindEqualIndices()) {
      auto raw = sqlite_utils::ExtractSqliteValue<NumericType>(value);
      index->IntersectRows(accessor_.EqualIndices(raw));
      return;
    }

    if (kIsIntegralType && (type == SQLITE_INTEGER || type == SQLITE_NULL)) {
      FilterWithCast<int64_t>(op, value, index);
    } else if (type == SQLITE_INTEGER || type == SQLITE_FLOAT ||
               type == SQLITE_NULL) {
      FilterWithCast<double>(op, value, index);
    } else {
      PERFETTO_FATAL("Unexpected sqlite value to compare against");
    }
  }

  Comparator Sort(const QueryConstraints::OrderBy& ob) const override {
    if (ob.desc) {
      return [this](uint32_t f, uint32_t s) {
        return sqlite_utils::CompareValuesDesc(accessor_.Get(f),
                                               accessor_.Get(s));
      };
    }
    return [this](uint32_t f, uint32_t s) {
      return sqlite_utils::CompareValuesAsc(accessor_.Get(f), accessor_.Get(s));
    };
  }

  bool HasOrdering() const override { return accessor_.HasOrdering(); }

  Table::ColumnType GetType() const override {
    if (std::is_same<NumericType, int32_t>::value) {
      return Table::ColumnType::kInt;
    } else if (std::is_same<NumericType, uint8_t>::value ||
               std::is_same<NumericType, uint32_t>::value) {
      return Table::ColumnType::kUint;
    } else if (std::is_same<NumericType, int64_t>::value) {
      return Table::ColumnType::kLong;
    } else if (std::is_same<NumericType, double>::value) {
      return Table::ColumnType::kDouble;
    }
    PERFETTO_FATAL("Unexpected column type");
  }

 private:
  static constexpr bool kIsIntegralType = std::is_integral<NumericType>::value;
  static constexpr bool kIsRealType =
      std::is_floating_point<NumericType>::value;

  NumericType kTMin = std::numeric_limits<NumericType>::lowest();
  NumericType kTMax = std::numeric_limits<NumericType>::max();

  // Filters the rows of this column by creating the predicate from the sqlite
  // value using type |UpcastNumericType| and casting data from the column
  // to also be this type.
  // Note: We cast here to make numeric comparisions as accurate as possible.
  // For example, suppose NumericType == uint32_t and the sqlite value has
  // an integer. Then UpcastNumericType == int64_t because uint32_t can be
  // upcast to an int64_t and it's the most generic type we can compare using.
  // Alternatively if either the column or sqlite value is real, we will always
  // cast to a double before comparing.
  template <typename UpcastNumericType>
  void FilterWithCast(int op,
                      sqlite3_value* value,
                      FilteredRowIndex* index) const {
    auto predicate =
        sqlite_utils::CreateNumericPredicate<UpcastNumericType>(op, value);
    auto cast_predicate = [this,
                           predicate](uint32_t row) PERFETTO_ALWAYS_INLINE {
      return predicate(static_cast<UpcastNumericType>(accessor_.Get(row)));
    };
    index->FilterRows(cast_predicate);
  }

  Accessor accessor_;
};

// Defines an accessor for columns.
// An accessor is a abstraction over the method to retrieve data in a column. As
// there are many possible types of backing data (std::vector, std::deque,
// creating on the flight etc.), this class hides this complexity behind an
// interface to let the column implementation focus on actually interfacing
// with SQLite and rest of trace processor.
// This class exists as an interface for documentation purposes. There should
// be no use of it apart from classes inheriting from it to ensure they comply
// with the requirements of the interface.
template <typename DataType>
class Accessor {
 public:
  using Type = DataType;

  virtual ~Accessor() = default;

  // Returns the number of elements in the backing storage.
  virtual uint32_t Size() const = 0;

  // Returns the element located at index |idx|.
  virtual Type Get(uint32_t idx) const = 0;

  // Returns whether the backing data source is ordered. |LowerBoundIndex| and
  // |UpperBoundIndex| will be called only if HasOrdering() returns true.
  virtual bool HasOrdering() const { return false; }

  // Returns the index of the lower bound of the value.
  virtual uint32_t LowerBoundIndex(Type) const { PERFETTO_CHECK(false); }

  // Returns the index of the lower bound of the value.
  virtual uint32_t UpperBoundIndex(Type) const { PERFETTO_CHECK(false); }

  // Returns whether the backing data sources can efficiently provide the
  // indices of elements equal to a given value. |EqualIndices| will be called
  // only if |CanFindEqualIndices| returns true.
  virtual bool CanFindEqualIndices() const { return false; }

  // Returns the indices into the backing data source with value equal to
  // |value|.
  virtual std::vector<uint32_t> EqualIndices(Type) const {
    PERFETTO_CHECK(false);
  }
};

// An accessor implementation for string which uses a deque to store offsets
// into a StringPool.
class StringPoolAccessor : public Accessor<NullTermStringView> {
 public:
  StringPoolAccessor(const std::deque<StringPool::Id>* deque,
                     const StringPool* string_pool);
  ~StringPoolAccessor() override;

  uint32_t Size() const override {
    return static_cast<uint32_t>(deque_->size());
  }

  NullTermStringView Get(uint32_t idx) const override {
    return string_pool_->Get((*deque_)[idx]);
  }

 private:
  const std::deque<StringPool::Id>* deque_;
  const StringPool* string_pool_;
};

// An accessor implementation for string which uses a deque to store indices
// into a vector of strings.
template <typename Id>
class StringVectorAccessor : public Accessor<NullTermStringView> {
 public:
  StringVectorAccessor(const std::deque<Id>* deque,
                       const std::vector<const char*>* string_map)
      : deque_(deque), string_map_(string_map) {}
  ~StringVectorAccessor() override = default;

  uint32_t Size() const override {
    return static_cast<uint32_t>(deque_->size());
  }

  NullTermStringView Get(uint32_t idx) const override {
    const char* ptr = (*string_map_)[(*deque_)[idx]];
    return ptr ? NullTermStringView(ptr) : NullTermStringView();
  }

 private:
  const std::deque<Id>* deque_;
  const std::vector<const char*>* string_map_;
};

// An accessor implementation for numeric columns which uses a deque as the
// backing storage with an opitonal index for quick equality filtering.
template <typename NumericType>
class NumericDequeAccessor : public Accessor<NumericType> {
 public:
  NumericDequeAccessor(const std::deque<NumericType>* deque,
                       const std::deque<std::vector<uint32_t>>* index,
                       bool has_ordering)
      : deque_(deque), index_(index), has_ordering_(has_ordering) {}
  ~NumericDequeAccessor() override = default;

  uint32_t Size() const override {
    return static_cast<uint32_t>(deque_->size());
  }

  NumericType Get(uint32_t idx) const override { return (*deque_)[idx]; }

  bool HasOrdering() const override { return has_ordering_; }

  uint32_t LowerBoundIndex(NumericType value) const override {
    PERFETTO_DCHECK(HasOrdering());
    auto it = std::lower_bound(deque_->begin(), deque_->end(), value);
    return static_cast<uint32_t>(std::distance(deque_->begin(), it));
  }

  uint32_t UpperBoundIndex(NumericType value) const override {
    PERFETTO_DCHECK(HasOrdering());
    auto it = std::upper_bound(deque_->begin(), deque_->end(), value);
    return static_cast<uint32_t>(std::distance(deque_->begin(), it));
  }

  bool CanFindEqualIndices() const override {
    return std::is_integral<NumericType>::value && index_ != nullptr;
  }

  std::vector<uint32_t> EqualIndices(NumericType value) const override {
    PERFETTO_DCHECK(CanFindEqualIndices());
    if (value < 0 || static_cast<size_t>(value) >= index_->size())
      return {};
    return (*index_)[static_cast<size_t>(value)];
  }

 private:
  const std::deque<NumericType>* deque_ = nullptr;
  const std::deque<std::vector<uint32_t>>* index_ = nullptr;
  bool has_ordering_ = false;
};

class TsEndAccessor : public Accessor<int64_t> {
 public:
  TsEndAccessor(const std::deque<int64_t>* ts, const std::deque<int64_t>* dur);
  ~TsEndAccessor() override;

  uint32_t Size() const override { return static_cast<uint32_t>(ts_->size()); }

  int64_t Get(uint32_t idx) const override {
    return (*ts_)[idx] + (*dur_)[idx];
  }

 private:
  const std::deque<int64_t>* ts_ = nullptr;
  const std::deque<int64_t>* dur_ = nullptr;
};

class RowIdAccessor : public Accessor<int64_t> {
 public:
  RowIdAccessor(TableId table_id);
  ~RowIdAccessor() override;

  uint32_t Size() const override {
    return std::numeric_limits<uint32_t>::max();
  }

  int64_t Get(uint32_t idx) const override {
    return TraceStorage::CreateRowId(table_id_, idx);
  }

 private:
  TableId table_id_;
};

class RowAccessor : public Accessor<uint32_t> {
 public:
  RowAccessor();
  ~RowAccessor() override;

  uint32_t Size() const override {
    return std::numeric_limits<uint32_t>::max();
  }

  uint32_t Get(uint32_t idx) const override { return idx; }

  bool HasOrdering() const override { return true; }

  uint32_t LowerBoundIndex(uint32_t idx) const override { return idx; }

  uint32_t UpperBoundIndex(uint32_t idx) const override { return idx + 1; }
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_STORAGE_COLUMNS_H_
