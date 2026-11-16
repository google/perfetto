/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_SCATTER_MIPMAP_OPERATOR_SCATTER_MIPMAP_OPERATOR_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_SCATTER_MIPMAP_OPERATOR_SCATTER_MIPMAP_OPERATOR_H_

#include <sqlite3.h>
#include <cstdint>
#include <vector>

#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/plugins/scatter_mipmap_operator/scatter_mipmap_index.h"
#include "src/trace_processor/sqlite/bindings/sqlite_module.h"
#include "src/trace_processor/sqlite/module_state_manager.h"

namespace perfetto::trace_processor::scatter_mipmap_operator {

// ScatterMipmapOperator provides 2D spatial downsampling / mipmapping.
//
// Supported declaration forms:
// 1. Table form:
//    CREATE VIRTUAL TABLE t USING __intrinsic_scatter_mipmap(
//        table, id, x, y [, c [, categories_top_n]]);
//    - If `id` is '' or 'NULL', `id` defaults to the 0-based ordinal of the
//    row.
//    - If `table` is a dataframe or views an __intrinsic_* dataframe, fast
//      direct streaming is used without SQL execution.
//    - If `categories_top_n` is provided, it must be an integer N > 0. Top-N
//      categories by frequency are assigned indices 0..N-1, other non-null
//      values are mapped to N ("Other"), and NULL values are mapped to -1.
// 2. Subquery form:
//    CREATE VIRTUAL TABLE t USING __intrinsic_scatter_mipmap((SELECT ...));
//    - 2 columns: (x, y). `id` = 0-based ordinal, `c` = NULL / NaN.
//    - 3 columns: (x, y, c). `id` = 0-based ordinal.
//    - 4 columns: (id, x, y, c). If `id` is NULL, defaults to 0-based ordinal.
//
// Accepted types for x and y include any numeric types (INTEGER, REAL).
// Column c can be numeric or string/TEXT when categories_top_n is provided.
// Rows with NULL/non-finite x or y are skipped. NULL or non-finite c is stored
// as NaN (or -1 if categorical) and returned as NULL in query results.
//
// Querying:
//    SELECT id, x, y, c, count
//    FROM t(x_min, x_max, y_min, y_max, cols, rows [, hidden]);
//    - Returns one representative point per non-empty cell of a cols x rows
//      grid over the given bounds, with the exact number of points in it.
//    - `hidden` (optional TEXT) is a comma-separated list of category indexes
//      to exclude, e.g. '0,3,-1' (-1 = NULL c). Excluded points are not
//      counted and never chosen as representatives.
//    - cols = rows = 0 lists the categories:
//      SELECT out_cat_idx, out_cat_value, out_cat_count FROM t(...,0,0).
struct ScatterMipmapOperator : sqlite::Module<ScatterMipmapOperator> {
  struct State {
    ScatterMipmapIndex index;
  };
  struct Context : sqlite::ModuleStateManager<ScatterMipmapOperator> {
    explicit Context(PerfettoSqlConnection* _connection)
        : sqlite::ModuleStateManager<ScatterMipmapOperator>(
              owned_committed_store_),
          connection(_connection) {}
    PerfettoSqlConnection* connection;

   private:
    sqlite::CommittedStateManager owned_committed_store_;
  };
  struct Vtab : sqlite::Module<ScatterMipmapOperator>::Vtab {
    sqlite::ModuleStateManager<ScatterMipmapOperator>::PerVtabState* state;
  };
  struct Cursor : sqlite::Module<ScatterMipmapOperator>::Cursor {
    std::vector<ScatterMipmapIndex::Result> results;
    uint32_t current_index = 0;
    ScatterMipmapIndex::QueryBuffers buffers;
    bool is_listing_mode = false;
  };

  static constexpr auto kType = kCreateOnly;
  static constexpr bool kSupportsWrites = false;
  static constexpr bool kDoesOverloadFunctions = false;

  static int Create(sqlite3*,
                    void*,
                    int,
                    const char* const*,
                    sqlite3_vtab**,
                    char**);
  static int Destroy(sqlite3_vtab*);

  static int Connect(sqlite3*,
                     void*,
                     int,
                     const char* const*,
                     sqlite3_vtab**,
                     char**);
  static int Disconnect(sqlite3_vtab*);

  static int BestIndex(sqlite3_vtab*, sqlite3_index_info*);

  static int Open(sqlite3_vtab*, sqlite3_vtab_cursor**);
  static int Close(sqlite3_vtab_cursor*);

  static int Filter(sqlite3_vtab_cursor*,
                    int,
                    const char*,
                    int,
                    sqlite3_value**);
  static int Next(sqlite3_vtab_cursor*);
  static int Eof(sqlite3_vtab_cursor*);
  static int Column(sqlite3_vtab_cursor*, sqlite3_context*, int);
  static int Rowid(sqlite3_vtab_cursor*, sqlite_int64*);

  static int Begin(sqlite3_vtab*) { return SQLITE_OK; }
  static int Sync(sqlite3_vtab*) { return SQLITE_OK; }
  static int Commit(sqlite3_vtab*) { return SQLITE_OK; }
  static int Rollback(sqlite3_vtab*) { return SQLITE_OK; }
  static int Savepoint(sqlite3_vtab* t, int r) {
    ScatterMipmapOperator::Vtab* vtab = GetVtab(t);
    sqlite::ModuleStateManager<ScatterMipmapOperator>::OnSavepoint(vtab->state,
                                                                   r);
    return SQLITE_OK;
  }
  static int Release(sqlite3_vtab* t, int r) {
    ScatterMipmapOperator::Vtab* vtab = GetVtab(t);
    sqlite::ModuleStateManager<ScatterMipmapOperator>::OnRelease(vtab->state,
                                                                 r);
    return SQLITE_OK;
  }
  static int RollbackTo(sqlite3_vtab* t, int r) {
    ScatterMipmapOperator::Vtab* vtab = GetVtab(t);
    sqlite::ModuleStateManager<ScatterMipmapOperator>::OnRollbackTo(vtab->state,
                                                                    r);
    return SQLITE_OK;
  }

  static constexpr sqlite3_module kModule = CreateModule();
};

void RegisterPlugin();

}  // namespace perfetto::trace_processor::scatter_mipmap_operator

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_SCATTER_MIPMAP_OPERATOR_SCATTER_MIPMAP_OPERATOR_H_
