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

#include "src/trace_processor/thread_table.h"

#include "perfetto/base/logging.h"
#include "src/trace_processor/query_constraints.h"
#include "src/trace_processor/sqlite_utils.h"

namespace perfetto {
namespace trace_processor {

namespace {

using namespace sqlite_utils;

}  // namespace

ThreadTable::ThreadTable(sqlite3*, const TraceStorage* storage)
    : storage_(storage) {}

void ThreadTable::RegisterTable(sqlite3* db, const TraceStorage* storage) {
  Table::Register<ThreadTable>(db, storage, "thread");
}

base::Optional<Table::Schema> ThreadTable::Init(int, const char* const*) {
  return Schema(
      {
          Table::Column(Column::kUtid, "utid", ColumnType::kInt),
          Table::Column(Column::kUpid, "upid", ColumnType::kInt),
          Table::Column(Column::kName, "name", ColumnType::kString),
          Table::Column(Column::kTid, "tid", ColumnType::kInt),
          Table::Column(Column::kStartTs, "start_ts", ColumnType::kLong),
      },
      {Column::kUtid});
}

std::unique_ptr<Table::Cursor> ThreadTable::CreateCursor() {
  return std::unique_ptr<Table::Cursor>(new Cursor(this));
}

int ThreadTable::BestIndex(const QueryConstraints& qc, BestIndexInfo* info) {
  info->estimated_cost = static_cast<uint32_t>(storage_->thread_count());

  // If the query has a constraint on the |utid| field, return a reduced cost
  // because we can do that filter efficiently.
  const auto& constraints = qc.constraints();
  for (const auto& cs : qc.constraints()) {
    if (cs.iColumn == Column::kUtid) {
      info->estimated_cost = IsOpEq(constraints.front().op) ? 1 : 10;
    }
  }
  return SQLITE_OK;
}

ThreadTable::Cursor::Cursor(ThreadTable* table)
    : Table::Cursor(table), storage_(table->storage_), table_(table) {}

int ThreadTable::Cursor::Filter(const QueryConstraints& qc,
                                sqlite3_value** argv) {
  *this = Cursor(table_);

  min = 0;
  max = static_cast<uint32_t>(storage_->thread_count()) - 1;
  desc = false;
  current = min;
  for (size_t j = 0; j < qc.constraints().size(); j++) {
    const auto& cs = qc.constraints()[j];
    if (cs.iColumn == Column::kUtid) {
      UniqueTid constraint_utid =
          static_cast<UniqueTid>(sqlite3_value_int(argv[j]));
      // Filter the range of utids that we are interested in, based on the
      // constraints in the query. Everything between min and max (inclusive)
      // will be returned.
      if (IsOpEq(cs.op)) {
        min = constraint_utid;
        max = constraint_utid;
      } else if (IsOpGe(cs.op) || IsOpGt(cs.op)) {
        min = IsOpGt(cs.op) ? constraint_utid + 1 : constraint_utid;
      } else if (IsOpLe(cs.op) || IsOpLt(cs.op)) {
        max = IsOpLt(cs.op) ? constraint_utid - 1 : constraint_utid;
      }
    }
  }
  for (const auto& ob : qc.order_by()) {
    if (ob.iColumn == Column::kUtid) {
      desc = ob.desc;
      current = desc ? max : min;
    }
  }
  return SQLITE_OK;
}

int ThreadTable::Cursor::Column(sqlite3_context* context, int N) {
  const auto& thread = storage_->GetThread(current);
  switch (N) {
    case Column::kUtid: {
      sqlite3_result_int64(context, current);
      break;
    }
    case Column::kUpid: {
      if (thread.upid.has_value()) {
        sqlite3_result_int64(context, thread.upid.value());
      } else {
        sqlite3_result_null(context);
      }
      break;
    }
    case Column::kName: {
      const auto& name = storage_->GetString(thread.name_id);
      sqlite3_result_text(context, name.c_str(), -1, kSqliteStatic);
      break;
    }
    case Column::kTid: {
      sqlite3_result_int64(context, thread.tid);
      break;
    }
    case Column::kStartTs: {
      if (thread.start_ns != 0) {
        sqlite3_result_int64(context, thread.start_ns);
      } else {
        sqlite3_result_null(context);
      }
      break;
    }
    default: {
      PERFETTO_FATAL("Unknown column %d", N);
      break;
    }
  }
  return SQLITE_OK;
}

int ThreadTable::Cursor::Next() {
  if (desc) {
    --current;
  } else {
    ++current;
  }
  return SQLITE_OK;
}

int ThreadTable::Cursor::Eof() {
  return desc ? current < min : current > max;
}
}  // namespace trace_processor
}  // namespace perfetto
