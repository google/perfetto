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
#include <optional>
#include <string>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/sqlite/bindings/sqlite_module.h"

namespace perfetto::trace_processor {

class PerfettoSqlConnection;

// Runs a pipeline's optimized plan, serialized into the SQL which reads it:
// `__intrinsic_pipeline(X'...')`.
//
// The plan is all a pipeline needs to run, so SQL holding it works anywhere
// SQL can be stored and run later, with nothing to keep alive alongside it.
// Each cursor loads the plan when first filtered, and keeps it for as long as
// it is given the same plan.
//
// The table has a fixed width: pipelines output into generic columns `c0`,
// `c1`, ..., which the SQL reading them renames to the pipeline's own names.
struct PipelineModule : sqlite::Module<PipelineModule> {
  static constexpr auto kType = kEponymousOnly;
  static constexpr bool kSupportsWrites = false;
  static constexpr bool kDoesOverloadFunctions = false;
  static constexpr char kName[] = "__intrinsic_pipeline";
  // The most columns a pipeline can output.
  static constexpr uint32_t kMaxColumns = 256;

  struct Context {
    StringPool* pool;
    // Plans the pipelines this module runs.
    PerfettoSqlConnection* connection;
  };
  struct Vtab : sqlite::Module<PipelineModule>::Vtab {
    Context* context;
  };
  struct Cursor : sqlite::Module<PipelineModule>::Cursor {
    using ResultFn = void (*)(sqlite3_context*,
                              StringPool*,
                              const core::exec::ColumnView&,
                              uint32_t);
    struct ColumnReader {
      const core::exec::ColumnView* view;
      ResultFn result;
    };
    // The serialized plan `plan` was loaded from.
    std::string serialized;
    std::unique_ptr<pipeline::PhysicalPlan> plan;
    StringPool* pool = nullptr;
    std::unique_ptr<core::exec::RowCursor> rows;
    std::vector<ColumnReader> columns;
    bool eof = true;
    int64_t rowid = 0;
    // An output rowid lookup, applied after every tree fold.
    std::optional<int64_t> target_rowid;
  };

  // SQL reading `plan`'s output under its own column names.
  static base::StatusOr<std::string> SelectFrom(const pipeline::LogicalPlan&);

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
