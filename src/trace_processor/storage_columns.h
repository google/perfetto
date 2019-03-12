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

  // Returns whether this column is sorted in the storage.
  virtual bool IsNaturallyOrdered() const { return false; }

  const std::string& name() const { return col_name_; }
  bool hidden() const { return hidden_; }

 private:
  std::string col_name_;
  bool hidden_ = false;
};

// A column of numeric data backed by a deque.
template <typename T>
class NumericColumn : public StorageColumn {
 public:
  // |index| is an optional multimap which maps the values in deque
  // to the rows they are located at.
  NumericColumn(std::string col_name,
                const std::deque<T>* deque,
                const std::deque<std::vector<uint32_t>>* index,
                bool hidden,
                bool is_naturally_ordered)
      : StorageColumn(col_name, hidden),
        deque_(deque),
        index_(index),
        is_naturally_ordered_(is_naturally_ordered) {}

  void ReportResult(sqlite3_context* ctx, uint32_t row) const override {
    sqlite_utils::ReportSqliteResult(ctx, (*deque_)[row]);
  }

  Bounds BoundFilter(int op, sqlite3_value* sqlite_val) const override {
    Bounds bounds;
    bounds.max_idx = static_cast<uint32_t>(deque_->size());

    if (!is_naturally_ordered_)
      return bounds;

    // Makes the below code much more readable.
    using namespace sqlite_utils;

    T min = kTMin;
    T max = kTMax;
    if (IsOpGe(op) || IsOpGt(op)) {
      min = FindGtBound<T>(IsOpGe(op), sqlite_val);
    } else if (IsOpLe(op) || IsOpLt(op)) {
      max = FindLtBound<T>(IsOpLe(op), sqlite_val);
    } else if (IsOpEq(op)) {
      auto val = FindEqBound<T>(sqlite_val);
      min = val;
      max = val;
    }

    if (min <= kTMin && max >= kTMax)
      return bounds;

    // Convert the values into indices into the deque.
    auto min_it = std::lower_bound(deque_->begin(), deque_->end(), min);
    bounds.min_idx =
        static_cast<uint32_t>(std::distance(deque_->begin(), min_it));
    auto max_it = std::upper_bound(min_it, deque_->end(), max);
    bounds.max_idx =
        static_cast<uint32_t>(std::distance(deque_->begin(), max_it));
    bounds.consumed = true;

    return bounds;
  }

  void Filter(int op,
              sqlite3_value* value,
              FilteredRowIndex* index) const override {
    auto type = sqlite3_value_type(value);

    // If we are doing equality filtering on integers, try and use the index.
    bool is_int_compare = std::is_integral<T>::value && type == SQLITE_INTEGER;
    if (sqlite_utils::IsOpEq(op) && is_int_compare && index_ != nullptr) {
      FilterIntegerIndexEq(value, index);
      return;
    }

    bool is_null = type == SQLITE_NULL;
    if (std::is_integral<T>::value && (type == SQLITE_INTEGER || is_null)) {
      FilterWithCast<int64_t>(op, value, index);
    } else if (type == SQLITE_INTEGER || type == SQLITE_FLOAT || is_null) {
      FilterWithCast<double>(op, value, index);
    } else {
      PERFETTO_FATAL("Unexpected sqlite value to compare against");
    }
  }

  Comparator Sort(const QueryConstraints::OrderBy& ob) const override {
    if (ob.desc) {
      return [this](uint32_t f, uint32_t s) {
        return sqlite_utils::CompareValuesDesc((*deque_)[f], (*deque_)[s]);
      };
    }
    return [this](uint32_t f, uint32_t s) {
      return sqlite_utils::CompareValuesAsc((*deque_)[f], (*deque_)[s]);
    };
  }

  bool IsNaturallyOrdered() const override { return is_naturally_ordered_; }

  Table::ColumnType GetType() const override {
    if (std::is_same<T, int32_t>::value) {
      return Table::ColumnType::kInt;
    } else if (std::is_same<T, uint8_t>::value ||
               std::is_same<T, uint32_t>::value) {
      return Table::ColumnType::kUint;
    } else if (std::is_same<T, int64_t>::value) {
      return Table::ColumnType::kLong;
    } else if (std::is_same<T, double>::value) {
      return Table::ColumnType::kDouble;
    }
    PERFETTO_FATAL("Unexpected column type");
  }

 protected:
  const std::deque<T>* deque_ = nullptr;
  const std::deque<std::vector<uint32_t>>* index_ = nullptr;

 private:
  T kTMin = std::numeric_limits<T>::lowest();
  T kTMax = std::numeric_limits<T>::max();

  void FilterIntegerIndexEq(sqlite3_value* value,
                            FilteredRowIndex* index) const {
    auto raw = sqlite_utils::ExtractSqliteValue<int64_t>(value);
    if (raw < 0 || raw >= static_cast<int64_t>(index_->size())) {
      index->IntersectRows({});
      return;
    }
    index->IntersectRows((*index_)[static_cast<size_t>(raw)]);
  }

  template <typename C>
  void FilterWithCast(int op,
                      sqlite3_value* value,
                      FilteredRowIndex* index) const {
    auto predicate = sqlite_utils::CreateNumericPredicate<C>(op, value);
    index->FilterRows([this, &predicate](uint32_t row) {
      return predicate(static_cast<C>((*deque_)[row]));
    });
  }

  bool is_naturally_ordered_ = false;
};

template <typename Id>
class StringColumn final : public StorageColumn {
 public:
  StringColumn(std::string col_name,
               const std::deque<Id>* deque,
               const std::vector<std::string>* string_map,
               bool hidden = false)
      : StorageColumn(col_name, hidden),
        deque_(deque),
        string_map_(string_map) {}

  void ReportResult(sqlite3_context* ctx, uint32_t row) const override {
    const auto& str = (*string_map_)[(*deque_)[row]];
    if (str.empty()) {
      sqlite3_result_null(ctx);
    } else {
      sqlite3_result_text(ctx, str.c_str(), -1, sqlite_utils::kSqliteStatic);
    }
  }

  Bounds BoundFilter(int, sqlite3_value*) const override {
    Bounds bounds;
    bounds.max_idx = static_cast<uint32_t>(deque_->size());
    return bounds;
  }

  void Filter(int, sqlite3_value*, FilteredRowIndex*) const override {}

  Comparator Sort(const QueryConstraints::OrderBy& ob) const override {
    if (ob.desc) {
      return [this](uint32_t f, uint32_t s) {
        const std::string& a = (*string_map_)[(*deque_)[f]];
        const std::string& b = (*string_map_)[(*deque_)[s]];
        return sqlite_utils::CompareValuesDesc(a, b);
      };
    }
    return [this](uint32_t f, uint32_t s) {
      const std::string& a = (*string_map_)[(*deque_)[f]];
      const std::string& b = (*string_map_)[(*deque_)[s]];
      return sqlite_utils::CompareValuesAsc(a, b);
    };
  }

  Table::ColumnType GetType() const override {
    return Table::ColumnType::kString;
  }

  bool IsNaturallyOrdered() const override { return false; }

 private:
  const std::deque<Id>* deque_ = nullptr;
  const std::vector<std::string>* string_map_ = nullptr;
};

// Column which represents the "ts_end" column present in all time based
// tables. It is computed by adding together the values in two deques.
class TsEndColumn final : public StorageColumn {
 public:
  TsEndColumn(std::string col_name,
              const std::deque<int64_t>* ts_start,
              const std::deque<int64_t>* dur);
  ~TsEndColumn() override;

  void ReportResult(sqlite3_context*, uint32_t) const override;

  Bounds BoundFilter(int op, sqlite3_value* value) const override;

  void Filter(int op, sqlite3_value* value, FilteredRowIndex*) const override;

  Comparator Sort(const QueryConstraints::OrderBy& ob) const override;

  // Returns the type of this column.
  Table::ColumnType GetType() const override {
    return Table::ColumnType::kLong;
  }

  bool IsNaturallyOrdered() const override { return false; }

 private:
  const std::deque<int64_t>* ts_start_;
  const std::deque<int64_t>* dur_;
};

// Column which is used to reference the args table in other tables. That is,
// it acts as a "foreign key" into the args table.
class IdColumn final : public StorageColumn {
 public:
  IdColumn(std::string column_name, TableId table_id);
  ~IdColumn() override;

  void ReportResult(sqlite3_context* ctx, uint32_t row) const override {
    auto id = TraceStorage::CreateRowId(table_id_, row);
    sqlite_utils::ReportSqliteResult(ctx, id);
  }

  Bounds BoundFilter(int, sqlite3_value*) const override { return Bounds{}; }

  void Filter(int op,
              sqlite3_value* value,
              FilteredRowIndex* index) const override {
    auto predicate = sqlite_utils::CreateNumericPredicate<RowId>(op, value);
    index->FilterRows([this, &predicate](uint32_t row) {
      return predicate(TraceStorage::CreateRowId(table_id_, row));
    });
  }

  Comparator Sort(const QueryConstraints::OrderBy& ob) const override {
    if (ob.desc) {
      return [this](uint32_t f, uint32_t s) {
        auto a = TraceStorage::CreateRowId(table_id_, f);
        auto b = TraceStorage::CreateRowId(table_id_, s);
        return sqlite_utils::CompareValuesDesc(a, b);
      };
    }
    return [this](uint32_t f, uint32_t s) {
      auto a = TraceStorage::CreateRowId(table_id_, f);
      auto b = TraceStorage::CreateRowId(table_id_, s);
      return sqlite_utils::CompareValuesAsc(a, b);
    };
  }

  Table::ColumnType GetType() const override {
    return Table::ColumnType::kLong;
  }

  bool IsNaturallyOrdered() const override { return false; }

 private:
  TableId table_id_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_STORAGE_COLUMNS_H_
