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

#include "src/trace_processor/filtered_row_index.h"
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
    virtual void Filter(int op, sqlite3_value*, FilteredRowIndex*) const = 0;

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

 private:
  std::vector<std::unique_ptr<Column>> columns_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_STORAGE_SCHEMA_H_
