/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/perfetto_sql/intrinsics/functions/trees/tree_functions.h"

#include <memory>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/trees/tree_agg.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/trees/tree_builder_ops.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/trees/tree_utils.h"
#include "src/trace_processor/sqlite/bindings/sqlite_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"

namespace perfetto::trace_processor {

namespace {

// Materializes a tree builder into a table with _tree_id and _tree_parent_id.
struct TreeToTable : public sqlite::Function<TreeToTable> {
  static constexpr char kName[] = "__intrinsic_tree_to_table";
  static constexpr int kArgCount = 1;
  using UserData = StringPool;

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    PERFETTO_DCHECK(argc == kArgCount);
    SQLITE_ASSIGN_OR_RETURN(ctx, auto wrapper, GetTreeBuilder(argv[0]));
    StringPool* pool = GetUserData(ctx);
    SQLITE_ASSIGN_OR_RETURN(ctx, auto df, wrapper->Take().BuildDataframe(pool));
    return sqlite::result::UniquePointer(
        ctx, std::make_unique<dataframe::Dataframe>(std::move(df)), "TABLE");
  }
};

}  // namespace

base::Status RegisterTreeFunctions(PerfettoSqlEngine& engine,
                                   StringPool& pool) {
  RETURN_IF_ERROR(engine.RegisterAggregateFunction<TreeAgg>(&pool));
  RETURN_IF_ERROR(engine.RegisterFunction<TreeToTable>(&pool));
  RETURN_IF_ERROR(engine.RegisterFunction<TreeSelectOpFn>(nullptr));
  return engine.RegisterFunction<TreeFilterFn>(nullptr);
}

}  // namespace perfetto::trace_processor
