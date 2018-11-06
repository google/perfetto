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

#ifndef SRC_TRACE_PROCESSOR_STORAGE_SCHEMA_H_
#define SRC_TRACE_PROCESSOR_STORAGE_SCHEMA_H_

#include <algorithm>
#include <deque>

#include "src/trace_processor/sqlite_utils.h"
#include "src/trace_processor/storage_cursor.h"
#include "src/trace_processor/table.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

// Defines the schema for a table which is backed by concrete storage (i.e. does
// not generate data on the fly).
// Used by all tables which are backed by data in TraceStorage.
class StorageSchema {
 public:
  // A column of data backed by data storage.
  class Column : public StorageCursor::ColumnReporter {
   public:
    struct Bounds {
      uint32_t min_idx = 0;
      uint32_t max_idx = std::numeric_limits<uint32_t>::max();
      bool consumed = false;
    };
    using Predicate = std::function<bool(uint32_t)>;
    using Comparator = std::function<int(uint32_t, uint32_t)>;

    Column(std::string col_name, bool hidden);
    virtual ~Column() override;

    // Implements StorageCursor::ColumnReporter.
    virtual void ReportResult(sqlite3_context*, uint32_t) const override = 0;

    // Bounds a filter on this column between a minimum and maximum index.
    // Generally this is only possible if the column is sorted.
    virtual Bounds BoundFilter(int op, sqlite3_value* value) const = 0;

    // Given a SQLite operator and value for the comparision, returns a
    // predicate which takes in a row index and returns whether the row should
    // be returned.
    virtual Predicate Filter(int op, sqlite3_value* value) const = 0;

    // Given a order by constraint for this column, returns a comparator
    // function which compares data in this column at two indices.
    virtual Comparator Sort(const QueryConstraints::OrderBy& ob) const = 0;

    // Returns the type of this column.
    virtual Table::ColumnType GetType() const = 0;

    // Returns whether this column is sorted in the storage.
    virtual bool IsNaturallyOrdered() const = 0;

    const std::string& name() const { return col_name_; }
    bool hidden() const { return hidden_; }

   private:
    std::string col_name_;
    bool hidden_ = false;
  };

  // A column of numeric data backed by a deque.
  template <typename T>
  class NumericColumn final : public Column {
   public:
    NumericColumn(std::string col_name,
                  const std::deque<T>* deque,
                  bool hidden,
                  bool is_naturally_ordered)
        : Column(col_name, hidden),
          deque_(deque),
          is_naturally_ordered_(is_naturally_ordered) {}

    void ReportResult(sqlite3_context* ctx, uint32_t row) const override {
      sqlite_utils::ReportSqliteResult(ctx, (*deque_)[row]);
    }

    Bounds BoundFilter(int op, sqlite3_value* sqlite_val) const override {
      Bounds bounds;
      bounds.max_idx = static_cast<uint32_t>(deque_->size());

      if (!is_naturally_ordered_)
        return bounds;

      auto min = std::numeric_limits<T>::min();
      auto max = std::numeric_limits<T>::max();

      // Makes the below code much more readable.
      using namespace sqlite_utils;

      // Try and bound the min and max value based on the constraints.
      auto value = sqlite_utils::ExtractSqliteValue<T>(sqlite_val);
      if (IsOpGe(op) || IsOpGt(op)) {
        min = IsOpGe(op) ? value : value + 1;
      } else if (IsOpLe(op) || IsOpLt(op)) {
        max = IsOpLe(op) ? value : value - 1;
      } else if (IsOpEq(op)) {
        min = value;
        max = value;
      } else {
        // We cannot bound on this constraint.
        return bounds;
      }

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

    Predicate Filter(int op, sqlite3_value* value) const override {
      auto binary_op = sqlite_utils::GetPredicateForOp<T>(op);
      T extracted = sqlite_utils::ExtractSqliteValue<T>(value);
      return [this, binary_op, extracted](uint32_t idx) {
        return binary_op((*deque_)[idx], extracted);
      };
    }

    Comparator Sort(const QueryConstraints::OrderBy& ob) const override {
      if (ob.desc) {
        return [this](uint32_t f, uint32_t s) {
          T a = (*deque_)[f];
          T b = (*deque_)[s];
          return a > b ? -1 : (a < b ? 1 : 0);
        };
      }
      return [this](uint32_t f, uint32_t s) {
        T a = (*deque_)[f];
        T b = (*deque_)[s];
        return a < b ? -1 : (a > b ? 1 : 0);
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
      } else if (std::is_same<T, uint64_t>::value) {
        return Table::ColumnType::kUlong;
      } else if (std::is_same<T, double>::value) {
        return Table::ColumnType::kDouble;
      }
      PERFETTO_CHECK(false);
    }

   private:
    const std::deque<T>* deque_ = nullptr;
    bool is_naturally_ordered_ = false;
  };

  template <typename Id>
  class StringColumn final : public Column {
   public:
    StringColumn(std::string col_name,
                 const std::deque<Id>* deque,
                 const std::deque<std::string>* string_map,
                 bool hidden = false)
        : Column(col_name, hidden), deque_(deque), string_map_(string_map) {}

    void ReportResult(sqlite3_context* ctx, uint32_t row) const override {
      const auto& str = (*string_map_)[(*deque_)[row]];
      if (str.empty()) {
        sqlite3_result_null(ctx);
      } else {
        auto kStatic = static_cast<sqlite3_destructor_type>(0);
        sqlite3_result_text(ctx, str.c_str(), -1, kStatic);
      }
    }

    Bounds BoundFilter(int, sqlite3_value*) const override {
      Bounds bounds;
      bounds.max_idx = static_cast<uint32_t>(deque_->size());
      return bounds;
    }

    Predicate Filter(int, sqlite3_value*) const override {
      return [](uint32_t) { return true; };
    }

    Comparator Sort(const QueryConstraints::OrderBy& ob) const override {
      if (ob.desc) {
        return [this](uint32_t f, uint32_t s) {
          const std::string& a = (*string_map_)[(*deque_)[f]];
          const std::string& b = (*string_map_)[(*deque_)[s]];
          return a > b ? -1 : (a < b ? 1 : 0);
        };
      }
      return [this](uint32_t f, uint32_t s) {
        const std::string& a = (*string_map_)[(*deque_)[f]];
        const std::string& b = (*string_map_)[(*deque_)[s]];
        return a < b ? -1 : (a > b ? 1 : 0);
      };
    }

    Table::ColumnType GetType() const override {
      return Table::ColumnType::kString;
    }

    bool IsNaturallyOrdered() const override { return false; }

   private:
    const std::deque<Id>* deque_ = nullptr;
    const std::deque<std::string>* string_map_ = nullptr;
  };

  // Column which represents the "ts_end" column present in all time based
  // tables. It is computed by adding together the values in two deques.
  class TsEndColumn final : public Column {
   public:
    TsEndColumn(std::string col_name,
                const std::deque<uint64_t>* ts_start,
                const std::deque<uint64_t>* dur);
    virtual ~TsEndColumn() override;

    // Implements StorageCursor::ColumnReporter.
    void ReportResult(sqlite3_context*, uint32_t) const override;

    // Bounds a filter on this column between a minimum and maximum index.
    // Generally this is only possible if the column is sorted.
    Bounds BoundFilter(int op, sqlite3_value* value) const override;

    // Given a SQLite operator and value for the comparision, returns a
    // predicate which takes in a row index and returns whether the row should
    // be returned.
    Predicate Filter(int op, sqlite3_value* value) const override;

    // Given a order by constraint for this column, returns a comparator
    // function which compares data in this column at two indices.
    Comparator Sort(const QueryConstraints::OrderBy& ob) const override;

    // Returns the type of this column.
    Table::ColumnType GetType() const override {
      return Table::ColumnType::kUlong;
    }

    // Returns whether this column is sorted in the storage.
    bool IsNaturallyOrdered() const override { return false; }

   private:
    const std::deque<uint64_t>* ts_start_;
    const std::deque<uint64_t>* dur_;
  };

  StorageSchema();
  StorageSchema(std::vector<std::unique_ptr<Column>> columns);

  Table::Schema ToTableSchema(std::vector<std::string> primary_keys);

  size_t ColumnIndexFromName(const std::string& name);

  std::vector<const StorageCursor::ColumnReporter*> ToColumnReporters() const {
    std::vector<const StorageCursor::ColumnReporter*> defns;
    for (const auto& col : columns_)
      defns.emplace_back(col.get());
    return defns;
  }

  const Column& GetColumn(size_t idx) const { return *(columns_[idx]); }

  template <typename T>
  static std::unique_ptr<TsEndColumn> TsEndPtr(std::string column_name,
                                               const std::deque<T>* ts_start,
                                               const std::deque<T>* ts_end) {
    return std::unique_ptr<TsEndColumn>(
        new TsEndColumn(column_name, ts_start, ts_end));
  }

  template <typename T>
  static std::unique_ptr<NumericColumn<T>> NumericColumnPtr(
      std::string column_name,
      const std::deque<T>* deque,
      bool hidden = false,
      bool is_naturally_ordered = false) {
    return std::unique_ptr<NumericColumn<T>>(
        new NumericColumn<T>(column_name, deque, hidden, is_naturally_ordered));
  }

  template <typename Id>
  static std::unique_ptr<StringColumn<Id>> StringColumnPtr(
      std::string column_name,
      const std::deque<Id>* deque,
      const std::deque<std::string>* lookup_map,
      bool hidden = false) {
    return std::unique_ptr<StringColumn<Id>>(
        new StringColumn<Id>(column_name, deque, lookup_map, hidden));
  }

 private:
  std::vector<std::unique_ptr<Column>> columns_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_STORAGE_SCHEMA_H_
