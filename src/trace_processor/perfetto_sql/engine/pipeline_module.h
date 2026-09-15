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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_PIPELINE_MODULE_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_PIPELINE_MODULE_H_

#include <sqlite3.h>

#include <cstdint>
#include <memory>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/perfetto_sql/pipeline/pipeline_plan.h"
#include "src/trace_processor/sqlite/bindings/sqlite_module.h"

namespace perfetto::trace_processor {

// Serves the rows of one pipeline to SQLite, streamed from the batch executor.
//
// Each pipeline is registered as a module of its own, because a module's
// columns are fixed and every pipeline has different ones. The module is
// eponymous, so registering it is all it takes for `SELECT * FROM <module>`
// to work: nothing is added to the schema and nothing has to be dropped.
struct PipelineModule : sqlite::Module<PipelineModule> {
  static constexpr auto kType = kEponymousOnly;
  static constexpr bool kSupportsWrites = false;
  static constexpr bool kDoesOverloadFunctions = false;

  struct Context {
    std::unique_ptr<pipeline::PipelinePlan> plan;
    StringPool* pool;
  };
  struct Vtab : sqlite::Module<PipelineModule>::Vtab {
    const Context* context;
  };
  struct Cursor : sqlite::Module<PipelineModule>::Cursor {
    std::unique_ptr<core::exec::RowCursor> rows;
    bool eof = true;
    int64_t rowid = 0;
  };

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

  // This needs to happen at the end as it depends on the functions
  // defined above.
  static constexpr sqlite3_module kModule = CreateModule();
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_PIPELINE_MODULE_H_
