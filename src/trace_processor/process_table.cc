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

#include "src/trace_processor/process_table.h"

#include "perfetto/base/logging.h"
#include "src/trace_processor/query_constraints.h"
#include "src/trace_processor/sqlite_utils.h"

namespace perfetto {
namespace trace_processor {

namespace {

using namespace sqlite_utils;

}  // namespace

ProcessTable::ProcessTable(sqlite3*, const TraceStorage* storage)
    : storage_(storage) {}

void ProcessTable::RegisterTable(sqlite3* db, const TraceStorage* storage) {
  SqliteTable::Register<ProcessTable>(db, storage, "process");
}

util::Status ProcessTable::Init(int, const char* const*, Schema* schema) {
  *schema = Schema(
      {
          SqliteTable::Column(Column::kUpid, "upid", ColumnType::kInt),
          SqliteTable::Column(Column::kName, "name", ColumnType::kString),
          SqliteTable::Column(Column::kPid, "pid", ColumnType::kUint),
          SqliteTable::Column(Column::kStartTs, "start_ts", ColumnType::kLong),
          SqliteTable::Column(Column::kEndTs, "end_ts", ColumnType::kLong),
          SqliteTable::Column(Column::kParentUpid, "parent_upid",
                              ColumnType::kInt),
      },
      {Column::kUpid});
  return util::OkStatus();
}

std::unique_ptr<SqliteTable::Cursor> ProcessTable::CreateCursor() {
  return std::unique_ptr<SqliteTable::Cursor>(new Cursor(this));
}

int ProcessTable::BestIndex(const QueryConstraints& qc, BestIndexInfo* info) {
  // If the query has a constraint on the |upid| field, return a reduced cost
  // because we can do that filter efficiently.
  const auto& cs = qc.constraints();
  auto fn = [](const QueryConstraints::Constraint& c) {
    return c.iColumn == Column::kUpid && sqlite_utils::IsOpEq(c.op);
  };
  info->estimated_cost = std::find_if(cs.begin(), cs.end(), fn) != cs.end()
                             ? 1
                             : static_cast<uint32_t>(storage_->process_count());
  return SQLITE_OK;
}

ProcessTable::Cursor::Cursor(ProcessTable* table)
    : SqliteTable::Cursor(table), storage_(table->storage_) {}

int ProcessTable::Cursor::Filter(const QueryConstraints& qc,
                                 sqlite3_value** argv) {
  min = 0;
  max = static_cast<uint32_t>(storage_->process_count()) - 1;
  desc = false;

  for (size_t j = 0; j < qc.constraints().size(); j++) {
    const auto& cs = qc.constraints()[j];
    if (cs.iColumn == Column::kUpid) {
      auto constraint_upid = static_cast<UniquePid>(sqlite3_value_int(argv[j]));
      // Set the range of upids that we are interested in, based on the
      // constraints in the query. Everything between min and max (inclusive)
      // will be returned.
      if (IsOpEq(cs.op)) {
        min = constraint_upid;
        max = constraint_upid;
      } else if (IsOpGe(cs.op) || IsOpGt(cs.op)) {
        min = IsOpGt(cs.op) ? constraint_upid + 1 : constraint_upid;
      } else if (IsOpLe(cs.op) || IsOpLt(cs.op)) {
        max = IsOpLt(cs.op) ? constraint_upid - 1 : constraint_upid;
      }
    }
  }

  for (const auto& ob : qc.order_by()) {
    if (ob.iColumn == Column::kUpid) {
      desc = ob.desc;
    }
  }
  current = desc ? max : min;

  return SQLITE_OK;
}

int ProcessTable::Cursor::Column(sqlite3_context* context, int N) {
  switch (N) {
    case Column::kUpid: {
      sqlite3_result_int64(context, current);
      break;
    }
    case Column::kName: {
      const auto& process = storage_->GetProcess(current);
      const auto& name = storage_->GetString(process.name_id);
      sqlite3_result_text(context, name.c_str(), -1, kSqliteStatic);
      break;
    }
    case Column::kPid: {
      const auto& process = storage_->GetProcess(current);
      sqlite3_result_int64(context, process.pid);
      break;
    }
    case Column::kStartTs: {
      const auto& process = storage_->GetProcess(current);
      if (process.start_ns != 0) {
        sqlite3_result_int64(context, process.start_ns);
      } else {
        sqlite3_result_null(context);
      }
      break;
    }
    case Column::kEndTs: {
      const auto& process = storage_->GetProcess(current);
      if (process.end_ns != 0) {
        sqlite3_result_int64(context, process.end_ns);
      } else {
        sqlite3_result_null(context);
      }
      break;
    }
    case Column::kParentUpid: {
      const auto& process = storage_->GetProcess(current);
      if (process.parent_upid.has_value()) {
        sqlite3_result_int64(context, process.parent_upid.value());
      } else {
        sqlite3_result_null(context);
      }
      break;
    }
    default:
      PERFETTO_FATAL("Unknown column %d", N);
      break;
  }
  return SQLITE_OK;
}

int ProcessTable::Cursor::Next() {
  if (desc) {
    --current;
  } else {
    ++current;
  }
  return SQLITE_OK;
}

int ProcessTable::Cursor::Eof() {
  return desc ? current < min : current > max;
}

}  // namespace trace_processor
}  // namespace perfetto
