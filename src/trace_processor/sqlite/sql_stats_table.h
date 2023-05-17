/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_SQLITE_SQL_STATS_TABLE_H_
#define SRC_TRACE_PROCESSOR_SQLITE_SQL_STATS_TABLE_H_

#include <limits>
#include <memory>

#include "perfetto/base/status.h"
#include "src/trace_processor/sqlite/sqlite_table.h"

namespace perfetto {
namespace trace_processor {

class QueryConstraints;
class TraceStorage;

// A virtual table that allows to introspect performances of the SQL engine
// for the kMaxLogEntries queries.
class SqlStatsTable final
    : public TypedSqliteTable<SqlStatsTable, const TraceStorage*> {
 public:
  enum Column {
    kQuery = 0,
    kTimeStarted = 1,
    kTimeFirstNext = 2,
    kTimeEnded = 3,
  };

  // Implementation of the SQLite cursor interface.
  class Cursor final : public SqliteTable::BaseCursor {
   public:
    explicit Cursor(SqlStatsTable* storage);
    ~Cursor() final;

    // Implementation of SqliteTable::Cursor.
    base::Status Filter(const QueryConstraints&,
                        sqlite3_value**,
                        FilterHistory);
    base::Status Next();
    bool Eof();
    base::Status Column(sqlite3_context*, int N);

   private:
    Cursor(Cursor&) = delete;
    Cursor& operator=(const Cursor&) = delete;

    Cursor(Cursor&&) noexcept = default;
    Cursor& operator=(Cursor&&) = default;

    size_t row_ = 0;
    size_t num_rows_ = 0;
    const TraceStorage* storage_ = nullptr;
    SqlStatsTable* table_ = nullptr;
  };

  SqlStatsTable(sqlite3*, const TraceStorage* storage);
  ~SqlStatsTable() final;

  // Table implementation.
  base::Status Init(int, const char* const*, Schema*) final;
  std::unique_ptr<SqliteTable::BaseCursor> CreateCursor() final;
  int BestIndex(const QueryConstraints&, BestIndexInfo*) final;

 private:
  const TraceStorage* const storage_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SQLITE_SQL_STATS_TABLE_H_
