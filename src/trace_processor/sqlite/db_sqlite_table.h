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

#ifndef SRC_TRACE_PROCESSOR_SQLITE_DB_SQLITE_TABLE_H_
#define SRC_TRACE_PROCESSOR_SQLITE_DB_SQLITE_TABLE_H_

#include "src/trace_processor/db/table.h"
#include "src/trace_processor/sqlite/query_cache.h"
#include "src/trace_processor/sqlite/sqlite_table.h"

namespace perfetto {
namespace trace_processor {

// Implements the SQLite table interface for db tables.
class DbSqliteTable : public SqliteTable {
 public:
  class Cursor : public SqliteTable::Cursor {
   public:
    Cursor(SqliteTable*, QueryCache*, const Table*);

    Cursor(Cursor&&) noexcept = default;
    Cursor& operator=(Cursor&&) = default;

    // Implementation of SqliteTable::Cursor.
    int Filter(const QueryConstraints& qc,
               sqlite3_value** argv,
               FilterHistory) override;
    int Next() override;
    int Eof() override;
    int Column(sqlite3_context*, int N) override;

   protected:
    // Sets the table this class uses as the reference for all filter
    // operations. Should be immediately followed by a call to Filter with
    // |FilterHistory::kDifferent|.
    void set_table(const Table* table) { initial_db_table_ = table; }

   private:
    enum class Mode {
      kSingleRow,
      kTable,
    };

    // Tries to create a sorted table to cache in |sorted_cache_table_| if the
    // constraint set matches the requirements.
    void TryCacheCreateSortedTable(const QueryConstraints&, FilterHistory);

    const Table* SourceTable() const {
      // Try and use the sorted cache table (if it exists) to speed up the
      // sorting. Otherwise, just use the original table.
      return sorted_cache_table_ ? &*sorted_cache_table_ : initial_db_table_;
    }

    Cursor(const Cursor&) = delete;
    Cursor& operator=(const Cursor&) = delete;

    QueryCache* cache_ = nullptr;
    const Table* initial_db_table_ = nullptr;

    // Only valid for Mode::kSingleRow.
    base::Optional<uint32_t> single_row_;

    // Only valid for Mode::kTable.
    base::Optional<Table> db_table_;
    base::Optional<Table::Iterator> iterator_;

    bool eof_ = true;

    // Stores a sorted version of |db_table_| sorted on a repeated equals
    // constraint. This allows speeding up repeated subqueries in joins
    // significantly.
    std::shared_ptr<Table> sorted_cache_table_;

    // Stores the count of repeated equality queries to decide whether it is
    // wortwhile to sort |db_table_| to create |sorted_cache_table_|.
    uint32_t repeated_cache_count_ = 0;

    Mode mode_ = Mode::kSingleRow;

    std::vector<Constraint> constraints_;
    std::vector<Order> orders_;
  };
  struct QueryCost {
    double cost;
    uint32_t rows;
  };
  struct Context {
    QueryCache* cache;
    const Table* table;
  };

  static void RegisterTable(sqlite3* db,
                            QueryCache* cache,
                            const Table* table,
                            const std::string& name);

  DbSqliteTable(sqlite3*, Context context);
  virtual ~DbSqliteTable() override;

  // Table implementation.
  util::Status Init(int,
                    const char* const*,
                    SqliteTable::Schema*) override final;
  std::unique_ptr<SqliteTable::Cursor> CreateCursor() override;
  int ModifyConstraints(QueryConstraints*) override;
  int BestIndex(const QueryConstraints&, BestIndexInfo*) override;

  static SqliteTable::Schema ComputeSchema(const Table& table,
                                           const char* table_name);

  // static for testing.
  static QueryCost EstimateCost(const Table& table, const QueryConstraints& qc);

 private:
  QueryCache* cache_ = nullptr;
  const Table* table_ = nullptr;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SQLITE_DB_SQLITE_TABLE_H_
