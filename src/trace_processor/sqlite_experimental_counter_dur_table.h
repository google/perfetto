/*
 * Copyright (C) 2020 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_SQLITE_EXPERIMENTAL_COUNTER_DUR_TABLE_H_
#define SRC_TRACE_PROCESSOR_SQLITE_EXPERIMENTAL_COUNTER_DUR_TABLE_H_

#include "src/trace_processor/sqlite/db_sqlite_table.h"

#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto {
namespace trace_processor {

class SqliteExperimentalCounterDurTable : public SqliteTable {
 public:
  struct Context {
    QueryCache* cache;
    const tables::CounterTable* table;
  };

  struct TableAndColumn {
    Table table;
    SparseVector<int64_t> dur;
  };

  class Cursor : public DbSqliteTable::Cursor {
   public:
    Cursor(SqliteTable*,
           QueryCache* cache,
           std::unique_ptr<TableAndColumn> table_and_column);
    ~Cursor() override;

   private:
    std::unique_ptr<TableAndColumn> table_and_column_;
  };

  SqliteExperimentalCounterDurTable(sqlite3*, Context context);
  ~SqliteExperimentalCounterDurTable() override;

  static void RegisterTable(sqlite3* db,
                            QueryCache* cache,
                            const tables::CounterTable& table);

  // SqliteTable implementation.
  util::Status Init(int,
                    const char* const*,
                    SqliteTable::Schema*) override final;
  std::unique_ptr<SqliteTable::Cursor> CreateCursor() override;
  int BestIndex(const QueryConstraints&, BestIndexInfo*) override;

  static SparseVector<int64_t> ComputeDurColumn(
      const tables::CounterTable& table);

 private:
  QueryCache* cache_ = nullptr;
  const tables::CounterTable* counter_table_ = nullptr;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SQLITE_EXPERIMENTAL_COUNTER_DUR_TABLE_H_
