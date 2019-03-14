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

#ifndef SRC_TRACE_PROCESSOR_STORAGE_TABLE_H_
#define SRC_TRACE_PROCESSOR_STORAGE_TABLE_H_

#include <set>

#include "src/trace_processor/row_iterators.h"
#include "src/trace_processor/storage_columns.h"
#include "src/trace_processor/storage_schema.h"
#include "src/trace_processor/table.h"

namespace perfetto {
namespace trace_processor {

// Base class for all table implementations which are backed by some data
// storage.
class StorageTable : public Table {
 public:
  // A cursor which abstracts common patterns found in storage backed tables. It
  // takes a strategy to iterate through rows and a column reporter for each
  // column to implement the Cursor interface.
  class Cursor final : public Table::Cursor {
   public:
    Cursor(std::unique_ptr<RowIterator>,
           std::vector<std::unique_ptr<StorageColumn>>*);

    // Implementation of Table::Cursor.
    int Next() override;
    int Eof() override;
    int Column(sqlite3_context*, int N) override;

   private:
    std::unique_ptr<RowIterator> iterator_;
    std::vector<std::unique_ptr<StorageColumn>>* columns_;
  };

  StorageTable();
  virtual ~StorageTable() override;

  // Table implementation.
  base::Optional<Table::Schema> Init(int, const char* const*) override final;
  std::unique_ptr<Table::Cursor> CreateCursor(const QueryConstraints&,
                                              sqlite3_value**) override;

  // Required methods for subclasses to implement.
  virtual StorageSchema CreateStorageSchema() = 0;
  virtual uint32_t RowCount() = 0;

 protected:
  const StorageSchema& schema() const { return schema_; }

  bool HasEqConstraint(const QueryConstraints&, const std::string& col_name);

 private:
  // Creates a row iterator which is optimized for a generic storage schema
  // (i.e. it does not make assumptions about values of columns).
  std::unique_ptr<RowIterator> CreateBestRowIterator(const QueryConstraints& qc,
                                                     sqlite3_value** argv);

  FilteredRowIndex CreateRangeIterator(
      const std::vector<QueryConstraints::Constraint>& cs,
      sqlite3_value** argv);

  std::pair<bool, bool> IsOrdered(
      const std::vector<QueryConstraints::OrderBy>& obs);

  std::vector<QueryConstraints::OrderBy> RemoveRedundantOrderBy(
      const std::vector<QueryConstraints::Constraint>& cs,
      const std::vector<QueryConstraints::OrderBy>& obs);

  std::vector<uint32_t> CreateSortedIndexVector(
      FilteredRowIndex index,
      const std::vector<QueryConstraints::OrderBy>& obs);

  StorageSchema schema_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_STORAGE_TABLE_H_
