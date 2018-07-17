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
#include "src/trace_processor/query_constraints.h"

#include "perfetto/base/logging.h"

namespace perfetto {
namespace trace_processor {

namespace {

inline bool IsOpEq(int op) {
  return op == SQLITE_INDEX_CONSTRAINT_EQ;
}

inline bool IsOpGe(int op) {
  return op == SQLITE_INDEX_CONSTRAINT_GE;
}

inline bool IsOpGt(int op) {
  return op == SQLITE_INDEX_CONSTRAINT_GT;
}

inline bool IsOpLe(int op) {
  return op == SQLITE_INDEX_CONSTRAINT_LE;
}

inline bool IsOpLt(int op) {
  return op == SQLITE_INDEX_CONSTRAINT_LT;
}

inline ProcessTable* AsTable(sqlite3_vtab* vtab) {
  return reinterpret_cast<ProcessTable*>(vtab);
}

}  // namespace

ProcessTable::ProcessTable(const TraceStorage* storage) : storage_(storage) {
  static_assert(offsetof(ProcessTable, base_) == 0,
                "SQLite base class must be first member of the table");
  memset(&base_, 0, sizeof(base_));
}

sqlite3_module ProcessTable::CreateModule() {
  sqlite3_module module;
  memset(&module, 0, sizeof(module));
  module.xConnect = [](sqlite3* db, void* raw_args, int, const char* const*,
                       sqlite3_vtab** tab, char**) {
    int res = sqlite3_declare_vtab(db,
                                   "CREATE TABLE processes("
                                   "upid UNSIGNED INT, "
                                   "name TEXT, "
                                   "PRIMARY KEY(upid)"
                                   ") WITHOUT ROWID;");
    if (res != SQLITE_OK)
      return res;
    TraceStorage* storage = static_cast<TraceStorage*>(raw_args);
    *tab = reinterpret_cast<sqlite3_vtab*>(new ProcessTable(storage));
    return SQLITE_OK;
  };
  module.xBestIndex = [](sqlite3_vtab* t, sqlite3_index_info* i) {
    return AsTable(t)->BestIndex(i);
  };
  module.xDisconnect = [](sqlite3_vtab* t) {
    delete AsTable(t);
    return SQLITE_OK;
  };
  module.xOpen = [](sqlite3_vtab* t, sqlite3_vtab_cursor** c) {
    return AsTable(t)->Open(c);
  };
  module.xClose = [](sqlite3_vtab_cursor* c) {
    delete AsCursor(c);
    return SQLITE_OK;
  };
  module.xFilter = [](sqlite3_vtab_cursor* c, int i, const char* s, int a,
                      sqlite3_value** v) {
    return AsCursor(c)->Filter(i, s, a, v);
  };
  module.xNext = [](sqlite3_vtab_cursor* c) { return AsCursor(c)->Next(); };
  module.xEof = [](sqlite3_vtab_cursor* c) { return AsCursor(c)->Eof(); };
  module.xColumn = [](sqlite3_vtab_cursor* c, sqlite3_context* a, int b) {
    return AsCursor(c)->Column(a, b);
  };
  module.xRowid = [](sqlite3_vtab_cursor*, sqlite_int64*) {
    return SQLITE_ERROR;
  };
  return module;
}

int ProcessTable::Open(sqlite3_vtab_cursor** ppCursor) {
  *ppCursor = reinterpret_cast<sqlite3_vtab_cursor*>(new Cursor(storage_));
  return SQLITE_OK;
}

// Called at least once but possibly many times before filtering things and is
// the best time to keep track of constriants.
int ProcessTable::BestIndex(sqlite3_index_info* idx) {
  QueryConstraints qc;

  for (int i = 0; i < idx->nOrderBy; i++) {
    Column column = static_cast<Column>(idx->aOrderBy[i].iColumn);
    unsigned char desc = idx->aOrderBy[i].desc;
    qc.AddOrderBy(column, desc);
  }
  idx->orderByConsumed = true;

  for (int i = 0; i < idx->nConstraint; i++) {
    const auto& cs = idx->aConstraint[i];
    if (!cs.usable)
      continue;
    qc.AddConstraint(cs.iColumn, cs.op);

    idx->estimatedCost = cs.iColumn == Column::kUpid ? 10 : 100;

    // argvIndex is 1-based so use the current size of the vector.
    int argv_index = static_cast<int>(qc.constraints().size());
    idx->aConstraintUsage[i].argvIndex = argv_index;
  }

  idx->idxStr = qc.ToNewSqlite3String().release();
  idx->needToFreeIdxStr = true;

  return SQLITE_OK;
}

ProcessTable::Cursor::Cursor(const TraceStorage* storage) : storage_(storage) {
  static_assert(offsetof(Cursor, base_) == 0,
                "SQLite base class must be first member of the cursor");
  memset(&base_, 0, sizeof(base_));
}

int ProcessTable::Cursor::Column(sqlite3_context* context, int N) {
  switch (N) {
    case Column::kUpid: {
      sqlite3_result_int64(context, current_upid_);
      break;
    }
    case Column::kName: {
      auto process = storage_->GetProcess(current_upid_);
      const auto& name = storage_->GetString(process.name_id);
      sqlite3_result_text(context, name.c_str(),
                          static_cast<int>(name.length()), nullptr);
      break;
    }
    default:
      PERFETTO_FATAL("Unknown column %d", N);
      break;
  }
  return SQLITE_OK;
}

int ProcessTable::Cursor::Filter(int /*idxNum*/,
                                 const char* idxStr,
                                 int argc,
                                 sqlite3_value** argv) {
  QueryConstraints qc = QueryConstraints::FromString(idxStr);

  PERFETTO_DCHECK(qc.constraints().size() == static_cast<size_t>(argc));

  min_upid_ = 1;
  max_upid_ = static_cast<uint32_t>(storage_->process_count());
  desc_ = false;
  current_upid_ = min_upid_;

  for (size_t j = 0; j < qc.constraints().size(); j++) {
    const auto& cs = qc.constraints()[j];
    if (cs.iColumn == Column::kUpid) {
      auto constraint_upid =
          static_cast<TraceStorage::UniquePid>(sqlite3_value_int(argv[j]));
      // Set the range of upids that we are interested in, based on the
      // constraints in the query. Everything between min and max (inclusive)
      // will be returned.
      if (IsOpGe(cs.op) || IsOpGt(cs.op)) {
        min_upid_ = IsOpGt(cs.op) ? constraint_upid + 1 : constraint_upid;
      } else if (IsOpLe(cs.op) || IsOpLt(cs.op)) {
        max_upid_ = IsOpLt(cs.op) ? constraint_upid - 1 : constraint_upid;
      } else if (IsOpEq(cs.op)) {
        min_upid_ = constraint_upid;
        max_upid_ = constraint_upid;
      }
    }
  }
  for (const auto& ob : qc.order_by()) {
    if (ob.iColumn == Column::kUpid) {
      desc_ = ob.desc;
      current_upid_ = desc_ ? max_upid_ : min_upid_;
    }
  }

  return SQLITE_OK;
}

int ProcessTable::Cursor::Next() {
  if (desc_) {
    --current_upid_;
  } else {
    ++current_upid_;
  }
  return SQLITE_OK;
}

int ProcessTable::Cursor::RowId(sqlite_int64* /* pRowid */) {
  return SQLITE_ERROR;
}

int ProcessTable::Cursor::Eof() {
  return desc_ ? current_upid_ < min_upid_ : current_upid_ > max_upid_;
}

}  // namespace trace_processor
}  // namespace perfetto
