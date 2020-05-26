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
#include "src/trace_processor/sqlite/sqlite_table.h"

namespace perfetto {
namespace trace_processor {

// Implements the SQLite table interface for db tables.
class DbSqliteTable : public SqliteTable {
 public:
  class Cursor final : public SqliteTable::Cursor {
   public:
    explicit Cursor(DbSqliteTable* table);

    Cursor(Cursor&&) noexcept = default;
    Cursor& operator=(Cursor&&) = default;

    // Implementation of SqliteTable::Cursor.
    int Filter(const QueryConstraints& qc,
               sqlite3_value** argv,
               FilterHistory) override;
    int Next() override;
    int Eof() override;
    int Column(sqlite3_context*, int N) override;

   private:
    Cursor(const Cursor&) = delete;
    Cursor& operator=(const Cursor&) = delete;

    const Table* initial_db_table_ = nullptr;

    base::Optional<Table> db_table_;
    base::Optional<Table::Iterator> iterator_;

    // Stores a sorted version of |db_table_| sorted on a repeated equals
    // constraint. This allows speeding up repeated subqueries in joins
    // significantly.
    base::Optional<Table> sorted_cache_table_;

    // Stores the count of repeated equality queries to decide whether it is
    // wortwhile to sort |db_table_| to create |sorted_cache_table_|.
    uint32_t repeated_cache_count_ = 0;

    std::vector<Constraint> constraints_;
    std::vector<Order> orders_;
  };
  struct QueryCost {
    double cost;
    uint32_t rows;
  };

  static void RegisterTable(sqlite3* db,
                            const Table* table,
                            const std::string& name);

  DbSqliteTable(sqlite3*, const Table* table);
  virtual ~DbSqliteTable() override;

  // Table implementation.
  util::Status Init(int,
                    const char* const*,
                    SqliteTable::Schema*) override final;
  std::unique_ptr<SqliteTable::Cursor> CreateCursor() override;
  int ModifyConstraints(QueryConstraints*) override;
  int BestIndex(const QueryConstraints&, BestIndexInfo*) override;

  // static for testing.
  static QueryCost EstimateCost(const Table& table, const QueryConstraints& qc);

 private:
  const Table* table_ = nullptr;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SQLITE_DB_SQLITE_TABLE_H_
