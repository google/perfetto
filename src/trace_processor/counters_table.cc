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

#include "src/trace_processor/counters_table.h"

#include "perfetto/base/logging.h"
#include "src/trace_processor/query_constraints.h"
#include "src/trace_processor/sqlite_utils.h"

namespace perfetto {
namespace trace_processor {

namespace {

using namespace sqlite_utils;

}  // namespace

CountersTable::CountersTable(sqlite3*, const TraceStorage* storage)
    : storage_(storage) {}

void CountersTable::RegisterTable(sqlite3* db, const TraceStorage* storage) {
  Table::Register<CountersTable>(db, storage, "counters");
}

std::string CountersTable::CreateTableStmt(int, const char* const*) {
  return "CREATE TABLE x("
         "ts UNSIGNED BIG INT, "
         "name text, "
         "value UNSIGNED BIG INT, "
         "dur UNSIGNED BIG INT, "
         "value_delta UNSIGNED BIG INT, "
         "ref UNSIGNED INT, "
         "ref_type TEXT, "
         "PRIMARY KEY(name, ts, ref)"
         ") WITHOUT ROWID;";
}

std::unique_ptr<Table::Cursor> CountersTable::CreateCursor() {
  return std::unique_ptr<Table::Cursor>(new Cursor(storage_));
}

int CountersTable::BestIndex(const QueryConstraints&, BestIndexInfo* info) {
  // TODO(taylori): Work out cost dependant on constraints.
  info->estimated_cost =
      static_cast<uint32_t>(storage_->counters().counter_count());
  return SQLITE_OK;
}

CountersTable::Cursor::Cursor(const TraceStorage* storage) : storage_(storage) {
  num_rows_ = storage->counters().counter_count();
}

int CountersTable::Cursor::Column(sqlite3_context* context, int N) {
  switch (N) {
    case Column::kTimestamp: {
      sqlite3_result_int64(
          context,
          static_cast<int64_t>(storage_->counters().timestamps()[row_]));
      break;
    }
    case Column::kValue: {
      sqlite3_result_int64(
          context, static_cast<int64_t>(storage_->counters().values()[row_]));
      break;
    }
    case Column::kName: {
      sqlite3_result_text(
          context,
          storage_->GetString(storage_->counters().name_ids()[row_]).c_str(),
          -1, nullptr);
      break;
    }
    case Column::kRef: {
      sqlite3_result_int64(
          context, static_cast<int64_t>(storage_->counters().refs()[row_]));
      break;
    }
    case Column::kRefType: {
      switch (storage_->counters().types()[row_]) {
        case RefType::kCPU_ID: {
          sqlite3_result_text(context, "cpu", -1, nullptr);
          break;
        }
        case RefType::kUTID: {
          sqlite3_result_text(context, "utid", -1, nullptr);
          break;
        }
      }
      break;
    }
    case Column::kDuration: {
      sqlite3_result_int64(
          context,
          static_cast<int64_t>(storage_->counters().durations()[row_]));
      break;
    }
    case Column::kValueDelta: {
      sqlite3_result_int64(
          context,
          static_cast<int64_t>(storage_->counters().value_deltas()[row_]));
      break;
    }
    default:
      PERFETTO_FATAL("Unknown column %d", N);
      break;
  }
  return SQLITE_OK;
}

int CountersTable::Cursor::Filter(const QueryConstraints&, sqlite3_value**) {
  return SQLITE_OK;
}

int CountersTable::Cursor::Next() {
  row_++;
  return SQLITE_OK;
}

int CountersTable::Cursor::Eof() {
  return row_ >= num_rows_;
}

}  // namespace trace_processor
}  // namespace perfetto
