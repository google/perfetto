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

#include "src/trace_processor/sqlite/sql_stats_table.h"

#include <sqlite3.h>

#include <algorithm>
#include <bitset>
#include <numeric>

#include "perfetto/base/status.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto {
namespace trace_processor {

SqlStatsTable::SqlStatsTable(sqlite3*, const TraceStorage* storage)
    : storage_(storage) {}
SqlStatsTable::~SqlStatsTable() = default;

base::Status SqlStatsTable::Init(int, const char* const*, Schema* schema) {
  *schema = Schema(
      {
          SqliteTable::Column(Column::kQuery, "query", SqlValue::Type::kString),
          SqliteTable::Column(Column::kTimeStarted, "started",
                              SqlValue::Type::kLong),
          SqliteTable::Column(Column::kTimeFirstNext, "first_next",
                              SqlValue::Type::kLong),
          SqliteTable::Column(Column::kTimeEnded, "ended",
                              SqlValue::Type::kLong),
      },
      {Column::kTimeStarted});
  return util::OkStatus();
}

std::unique_ptr<SqliteTable::BaseCursor> SqlStatsTable::CreateCursor() {
  return std::unique_ptr<SqliteTable::BaseCursor>(new Cursor(this));
}

int SqlStatsTable::BestIndex(const QueryConstraints&, BestIndexInfo*) {
  return SQLITE_OK;
}

SqlStatsTable::Cursor::Cursor(SqlStatsTable* table)
    : SqliteTable::BaseCursor(table),
      storage_(table->storage_),
      table_(table) {}
SqlStatsTable::Cursor::~Cursor() = default;

base::Status SqlStatsTable::Cursor::Filter(const QueryConstraints&,
                                           sqlite3_value**,
                                           FilterHistory) {
  *this = Cursor(table_);
  num_rows_ = storage_->sql_stats().size();
  return base::OkStatus();
}

base::Status SqlStatsTable::Cursor::Next() {
  row_++;
  return base::OkStatus();
}

bool SqlStatsTable::Cursor::Eof() {
  return row_ >= num_rows_;
}

base::Status SqlStatsTable::Cursor::Column(sqlite3_context* context, int col) {
  const TraceStorage::SqlStats& stats = storage_->sql_stats();
  switch (col) {
    case Column::kQuery:
      sqlite3_result_text(context, stats.queries()[row_].c_str(), -1,
                          sqlite_utils::kSqliteStatic);
      break;
    case Column::kTimeStarted:
      sqlite3_result_int64(context, stats.times_started()[row_]);
      break;
    case Column::kTimeFirstNext:
      sqlite3_result_int64(context, stats.times_first_next()[row_]);
      break;
    case Column::kTimeEnded:
      sqlite3_result_int64(context, stats.times_ended()[row_]);
      break;
  }
  return base::OkStatus();
}

}  // namespace trace_processor
}  // namespace perfetto
