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

#ifndef SRC_TRACE_PROCESSOR_SQLITE_EXPERIMENTAL_FLAMEGRAPH_TABLE_H_
#define SRC_TRACE_PROCESSOR_SQLITE_EXPERIMENTAL_FLAMEGRAPH_TABLE_H_

#include "src/trace_processor/sqlite/db_sqlite_table.h"

#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

class SqliteExperimentalFlamegraphTable : public SqliteTable {
 public:
  struct InputValues {
    int64_t ts;
    UniquePid upid;
    std::string profile_type;
  };

  class Cursor : public DbSqliteTable::Cursor {
   public:
    Cursor(SqliteTable*, TraceProcessorContext*);

    int Filter(const QueryConstraints& qc,
               sqlite3_value** argv,
               FilterHistory) override;

   private:
    TraceProcessorContext* context_ = nullptr;

    std::unique_ptr<Table> table_;
    InputValues values_ = {};
  };

  SqliteExperimentalFlamegraphTable(sqlite3*, TraceProcessorContext*);
  ~SqliteExperimentalFlamegraphTable() override;

  static void RegisterTable(sqlite3* db, TraceProcessorContext* storage);

  // SqliteTable implementation.
  util::Status Init(int,
                    const char* const*,
                    SqliteTable::Schema*) override final;
  std::unique_ptr<SqliteTable::Cursor> CreateCursor() override;
  int BestIndex(const QueryConstraints&, BestIndexInfo*) override;

 private:
  friend class Cursor;

  TraceProcessorContext* context_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SQLITE_EXPERIMENTAL_FLAMEGRAPH_TABLE_H_
