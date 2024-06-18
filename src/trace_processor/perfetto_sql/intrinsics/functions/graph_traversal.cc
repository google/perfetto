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

#include "src/trace_processor/perfetto_sql/intrinsics/functions/graph_traversal.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/tables_py.h"
#include "src/trace_processor/sqlite/bindings/sqlite_aggregate_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"

namespace perfetto::trace_processor {
namespace tables {
TreeTable::~TreeTable() = default;
}  // namespace tables

namespace {

struct Node {
  std::vector<uint32_t> dest_nodes;
  bool visited = false;
};

// An SQL aggregate-function which performs a DFS from a given start node in a
// graph and returns all the nodes which are reachable from the start node.
//
// Note: this function is not intended to be used directly from SQL: instead
// macros exist in the standard library, wrapping it and making it
// user-friendly.
struct Dfs : public SqliteAggregateFunction<Dfs> {
  static constexpr char kName[] = "__intrinsic_dfs";
  static constexpr int kArgCount = 3;
  using UserDataContext = StringPool;

  struct AggCtx : SqliteAggregateContext<AggCtx> {
    std::vector<Node> nodes;
    std::optional<uint32_t> start_id;
  };

  static void Step(sqlite3_context*, int argc, sqlite3_value** argv);
  static void Final(sqlite3_context* ctx);
};

void Dfs::Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
  PERFETTO_DCHECK(argc == kArgCount);

  auto& agg_ctx = AggCtx::GetOrCreateContextForStep(ctx);
  auto source = static_cast<uint32_t>(sqlite::value::Int64(argv[0]));
  auto dest = static_cast<uint32_t>(sqlite::value::Int64(argv[1]));

  uint32_t max = std::max(source, dest);
  if (max >= agg_ctx.nodes.size()) {
    agg_ctx.nodes.resize(max + 1);
  }
  agg_ctx.nodes[source].dest_nodes.push_back(dest);

  if (PERFETTO_UNLIKELY(!agg_ctx.start_id)) {
    agg_ctx.start_id = static_cast<uint32_t>(sqlite::value::Int64(argv[2]));
  }
}

void Dfs::Final(sqlite3_context* ctx) {
  auto raw_agg_ctx = AggCtx::GetContextOrNullForFinal(ctx);
  auto table = std::make_unique<tables::TreeTable>(GetUserData(ctx));
  if (auto* agg_ctx = raw_agg_ctx.get(); agg_ctx) {
    struct State {
      uint32_t id;
      std::optional<uint32_t> parent_id;
    };

    std::vector<State> stack{{*agg_ctx->start_id, std::nullopt}};
    while (!stack.empty()) {
      State state = stack.back();
      stack.pop_back();

      Node& node = agg_ctx->nodes[state.id];
      if (node.visited) {
        continue;
      }
      table->Insert({state.id, state.parent_id});
      node.visited = true;

      const auto& children = node.dest_nodes;
      for (auto it = children.rbegin(); it != children.rend(); ++it) {
        stack.emplace_back(State{*it, state.id});
      }
    }
  }
  return sqlite::result::RawPointer(ctx, table.release(), "TABLE",
                                    [](void* ptr) {
                                      std::unique_ptr<tables::TreeTable>(
                                          static_cast<tables::TreeTable*>(ptr));
                                    });
}

}  // namespace

base::Status RegisterGraphTraversalFunctions(PerfettoSqlEngine& engine,
                                             StringPool& string_pool) {
  return engine.RegisterSqliteAggregateFunction<Dfs>(&string_pool);
}

}  // namespace perfetto::trace_processor
