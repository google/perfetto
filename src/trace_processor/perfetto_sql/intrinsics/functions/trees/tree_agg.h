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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_FUNCTIONS_TREES_TREE_AGG_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_FUNCTIONS_TREES_TREE_AGG_H_

#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/runtime_dataframe_builder.h"
#include "src/trace_processor/sqlite/bindings/sqlite_aggregate_function.h"

namespace perfetto::trace_processor {

// Aggregate function that builds a Tree from rows with id/parent_id columns.
struct TreeAgg : public sqlite::AggregateFunction<TreeAgg> {
  static constexpr char kName[] = "__intrinsic_tree_agg";
  static constexpr int kArgCount = -1;
  using UserData = StringPool;

  struct AggCtx : sqlite::AggregateContext<AggCtx> {
    static constexpr int64_t kNullParentId =
        std::numeric_limits<int64_t>::max();

    std::unique_ptr<dataframe::RuntimeDataframeBuilder> builder;
    std::vector<int64_t> id_values;
    std::vector<int64_t> parent_id_values;
  };

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv);
  static void Final(sqlite3_context* ctx);
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_FUNCTIONS_TREES_TREE_AGG_H_
