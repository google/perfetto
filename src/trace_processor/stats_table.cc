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

#include "src/trace_processor/stats_table.h"

#include "src/trace_processor/sqlite_utils.h"

namespace perfetto {
namespace trace_processor {

StatsTable::StatsTable(sqlite3*, const TraceStorage* storage)
    : storage_(storage) {}

void StatsTable::RegisterTable(sqlite3* db, const TraceStorage* storage) {
  Table::Register<StatsTable>(db, storage, "stats");
}

base::Optional<Table::Schema> StatsTable::Init(int, const char* const*) {
  return Schema(
      {
          Table::Column(Column::kKey, "key", ColumnType::kString),
          Table::Column(Column::kValue, "value", ColumnType::kInt),
      },
      {Column::kKey});
}

std::unique_ptr<Table::Cursor> StatsTable::CreateCursor(const QueryConstraints&,
                                                        sqlite3_value**) {
  return std::unique_ptr<Table::Cursor>(new Cursor(storage_));
}

int StatsTable::BestIndex(const QueryConstraints&, BestIndexInfo*) {
  return SQLITE_OK;
}

StatsTable::Cursor::Cursor(const TraceStorage* storage) : storage_(storage) {}

int StatsTable::Cursor::Column(sqlite3_context* context, int N) {
  switch (N) {
    case Column::kKey:
      sqlite3_result_text(context, KeyForRow(row_), -1,
                          sqlite_utils::kSqliteStatic);
      break;
    case Column::kValue:
      sqlite3_result_int(context, ValueForRow(row_));
      break;
    default:
      PERFETTO_FATAL("Unknown column %d", N);
      break;
  }
  return SQLITE_OK;
}

int StatsTable::Cursor::Next() {
  ++row_;
  return SQLITE_OK;
}

int StatsTable::Cursor::Eof() {
  return row_ >= Row::kMax;
}

const char* StatsTable::Cursor::KeyForRow(uint8_t row) {
  switch (row) {
    case StatsTable::Row::kMismatchedSchedSwitch:
      return "mismatched_ss";
    case StatsTable::Row::kRssStatNoProcess:
      return "rss_stat_no_process";
    case StatsTable::Row::kMemCounterNoProcess:
      return "mem_count_no_process";
    default:
      PERFETTO_FATAL("Unknown row %u", row);
  }
}

int StatsTable::Cursor::ValueForRow(uint8_t row) {
  switch (row) {
    case StatsTable::Row::kMismatchedSchedSwitch: {
      auto val = storage_->stats().mismatched_sched_switch_tids;
      return static_cast<int>(val);
    }
    case StatsTable::Row::kRssStatNoProcess: {
      auto val = storage_->stats().rss_stat_no_process;
      return static_cast<int>(val);
    }
    case StatsTable::Row::kMemCounterNoProcess: {
      auto val = storage_->stats().mem_counter_no_process;
      return static_cast<int>(val);
    }
    default:
      PERFETTO_FATAL("Unknown row %u", row);
  }
}

}  // namespace trace_processor
}  // namespace perfetto
