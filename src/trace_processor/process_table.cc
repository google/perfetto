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

ProcessTable::ProcessTable(const TraceStorage* storage) : storage_(storage) {}

void ProcessTable::RegisterTable(sqlite3* db, const TraceStorage* storage) {
  Table::Register<ProcessTable>(db, storage,
                                "CREATE TABLE process("
                                "upid UNSIGNED INT, "
                                "name TEXT, "
                                "pid UNSIGNED INT, "
                                "PRIMARY KEY(upid)"
                                ") WITHOUT ROWID;");
}

std::unique_ptr<Table::Cursor> ProcessTable::CreateCursor() {
  return std::unique_ptr<Table::Cursor>(new Cursor(storage_));
}

int ProcessTable::BestIndex(const QueryConstraints& qc, BestIndexInfo* info) {
  for (const auto& constraint : qc.constraints()) {
    // Add a cost of 10 for filtering on upid (because we can do that
    // efficiently) and 100 otherwise.
    info->estimated_cost += constraint.iColumn == Column::kUpid ? 10 : 100;
  }
  return SQLITE_OK;
}

ProcessTable::Cursor::Cursor(const TraceStorage* storage) : storage_(storage) {}

int ProcessTable::Cursor::Column(sqlite3_context* context, int N) {
  switch (N) {
    case Column::kUpid: {
      sqlite3_result_int64(context, upid_filter_.current);
      break;
    }
    case Column::kName: {
      const auto& process = storage_->GetProcess(upid_filter_.current);
      const auto& name = storage_->GetString(process.name_id);
      sqlite3_result_text(context, name.c_str(),
                          static_cast<int>(name.length()), nullptr);
      break;
    }
    case Column::kPid: {
      const auto& process = storage_->GetProcess(upid_filter_.current);
      sqlite3_result_int64(context, process.pid);
      break;
    }
    default:
      PERFETTO_FATAL("Unknown column %d", N);
      break;
  }
  return SQLITE_OK;
}

int ProcessTable::Cursor::Filter(const QueryConstraints& qc,
                                 sqlite3_value** argv) {
  upid_filter_.min = 1;
  upid_filter_.max = static_cast<uint32_t>(storage_->process_count());
  upid_filter_.desc = false;
  upid_filter_.current = upid_filter_.min;

  for (size_t j = 0; j < qc.constraints().size(); j++) {
    const auto& cs = qc.constraints()[j];
    if (cs.iColumn == Column::kUpid) {
      auto constraint_upid = static_cast<UniquePid>(sqlite3_value_int(argv[j]));
      // Set the range of upids that we are interested in, based on the
      // constraints in the query. Everything between min and max (inclusive)
      // will be returned.
      if (IsOpGe(cs.op) || IsOpGt(cs.op)) {
        upid_filter_.min =
            IsOpGt(cs.op) ? constraint_upid + 1 : constraint_upid;
      } else if (IsOpLe(cs.op) || IsOpLt(cs.op)) {
        upid_filter_.max =
            IsOpLt(cs.op) ? constraint_upid - 1 : constraint_upid;
      } else if (IsOpEq(cs.op)) {
        upid_filter_.min = constraint_upid;
        upid_filter_.max = constraint_upid;
      }
    }
  }
  for (const auto& ob : qc.order_by()) {
    if (ob.iColumn == Column::kUpid) {
      upid_filter_.desc = ob.desc;
      upid_filter_.current =
          upid_filter_.desc ? upid_filter_.max : upid_filter_.min;
    }
  }

  return SQLITE_OK;
}

int ProcessTable::Cursor::Next() {
  if (upid_filter_.desc) {
    --upid_filter_.current;
  } else {
    ++upid_filter_.current;
  }
  return SQLITE_OK;
}

int ProcessTable::Cursor::Eof() {
  return upid_filter_.desc ? upid_filter_.current < upid_filter_.min
                           : upid_filter_.current > upid_filter_.max;
}

}  // namespace trace_processor
}  // namespace perfetto
