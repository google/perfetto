/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/perfetto_sql/intrinsics/functions/graph_helpers.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/perfetto_sql/intrinsics/types/node.h"
#include "src/trace_processor/sqlite/bindings/sqlite_aggregate_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"

namespace perfetto::trace_processor {
namespace {

struct AggCtx : SqliteAggregateContext<AggCtx> {
  perfetto_sql::Graph graph;
};

struct NodeAgg : public SqliteAggregateFunction<NodeAgg> {
  static constexpr char kName[] = "__intrinsic_graph_agg";
  static constexpr int kArgCount = 2;
  using UserDataContext = void;

  static void Step(sqlite3_context*, int argc, sqlite3_value** argv);
  static void Final(sqlite3_context* ctx);
};

void NodeAgg::Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
  PERFETTO_DCHECK(argc == kArgCount);

  auto source_id = static_cast<uint32_t>(sqlite::value::Int64(argv[0]));
  auto target_id = static_cast<uint32_t>(sqlite::value::Int64(argv[1]));
  uint32_t max_id = std::max(source_id, target_id);
  auto& agg_ctx = AggCtx::GetOrCreateContextForStep(ctx);
  if (max_id >= agg_ctx.graph.size()) {
    agg_ctx.graph.resize(max_id + 1);
  }
  agg_ctx.graph[source_id].outgoing_edges.push_back(target_id);
}

void NodeAgg::Final(sqlite3_context* ctx) {
  auto raw_agg_ctx = AggCtx::GetContextOrNullForFinal(ctx);
  if (!raw_agg_ctx.get()) {
    return;
  }
  auto nodes = std::make_unique<perfetto_sql::Graph>(
      std::move(raw_agg_ctx.get()->graph));
  return sqlite::result::UniquePointer(ctx, std::move(nodes), "GRAPH");
}

}  // namespace

base::Status RegisterGraphHelperFunctions(PerfettoSqlEngine& engine,
                                          StringPool& pool) {
  return engine.RegisterSqliteAggregateFunction<NodeAgg>(&pool);
}

}  // namespace perfetto::trace_processor
