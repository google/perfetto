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

#include "src/trace_processor/plugins/graph_traversal/graph_traversal.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/plugin/plugin.h"
#include "src/trace_processor/perfetto_sql/engine/dataframe_module.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/perfetto_sql/intrinsics/types/array.h"
#include "src/trace_processor/perfetto_sql/intrinsics/types/node.h"
#include "src/trace_processor/plugins/graph_traversal/tables_py.h"
#include "src/trace_processor/sqlite/bindings/sqlite_aggregate_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/graph.h"

namespace perfetto::trace_processor {

namespace {

using SearchFn = util::GraphTree (*)(const util::Digraph&,
                                     const std::vector<uint32_t>&);

// Searches `graph` from `start_ids`, either of which may be null. A start id
// outside the graph is a node without edges.
void Search(sqlite3_context* ctx,
            sqlite3_value** argv,
            const char* name,
            SearchFn search,
            bool reverse_start_ids,
            StringPool* pool) {
  auto* graph = sqlite::value::Pointer<perfetto_sql::Graph>(argv[0], "GRAPH");
  auto table = std::make_unique<tables::TreeTable>(pool);
  // A null graph means there were no edges: still traverse it (as an empty
  // graph) so that the start nodes themselves are returned.

  // If the array is empty, be forgiving and return an empty array. We could
  // return an error here but in 99% of cases, the caller will simply want
  // an empty table instead.
  auto* start_ids =
      sqlite::value::Pointer<perfetto_sql::IntArray>(argv[1], "ARRAY<LONG>");
  if (!start_ids) {
    return sqlite::result::UniquePointer(
        ctx,
        std::make_unique<dataframe::Dataframe>(std::move(table->dataframe())),
        "TABLE");
  }
  PERFETTO_DCHECK(!start_ids->empty());

  uint32_t graph_size = graph ? static_cast<uint32_t>(graph->size()) : 0;
  std::vector<uint32_t> seeds;
  std::vector<uint32_t> ids_outside_graph;
  base::FlatHashMap<uint32_t, uint32_t> node_outside_graph;
  for (int64_t x : *start_ids) {
    if (x < 0 || x > std::numeric_limits<int32_t>::max()) {
      return sqlite::result::Error(
          ctx, (std::string(name) +
                ": node ids must be non-negative 32-bit integers")
                   .c_str());
    }
    auto id = static_cast<uint32_t>(x);
    if (id >= graph_size) {
      auto [it, inserted] = node_outside_graph.Insert(
          id, graph_size + static_cast<uint32_t>(ids_outside_graph.size()));
      if (inserted) {
        ids_outside_graph.push_back(id);
      }
      id = *it;
    }
    seeds.push_back(id);
  }
  if (reverse_start_ids) {
    std::reverse(seeds.begin(), seeds.end());
  }

  util::Digraph digraph;
  digraph.offsets.reserve(graph_size + ids_outside_graph.size() + 1);
  for (uint32_t n = 0; n < graph_size; ++n) {
    const auto& edges = (*graph)[n].outgoing_edges;
    digraph.targets.insert(digraph.targets.end(), edges.begin(), edges.end());
    digraph.offsets.push_back(static_cast<uint32_t>(digraph.targets.size()));
  }
  digraph.offsets.resize(digraph.offsets.size() + ids_outside_graph.size(),
                         static_cast<uint32_t>(digraph.targets.size()));

  util::GraphTree tree = search(digraph, seeds);
  auto id_of = [&](uint32_t node) {
    return node < graph_size ? node : ids_outside_graph[node - graph_size];
  };
  for (uint32_t node : tree.order) {
    uint32_t parent = tree.parent[node];
    table->Insert({id_of(node), parent == util::GraphTree::kNoParent
                                    ? std::nullopt
                                    : std::make_optional(id_of(parent))});
  }
  return sqlite::result::UniquePointer(
      ctx,
      std::make_unique<dataframe::Dataframe>(std::move(table->dataframe())),
      "TABLE");
}

// An SQL aggregate-function which performs a DFS from a given start node in a
// graph and returns all the nodes which are reachable from the start node.
//
// Note: this function is not intended to be used directly from SQL: instead
// macros exist in the standard library, wrapping it and making it
// user-friendly.
struct Dfs : public sqlite::AggregateFunction<Dfs> {
  static constexpr char kName[] = "__intrinsic_dfs";
  static constexpr int kArgCount = 2;
  using UserData = StringPool;

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    PERFETTO_DCHECK(argc == kArgCount);
    // The last start id has always been searched from first.
    Search(ctx, argv, "DFS", &util::DepthFirstSearch,
           /*reverse_start_ids=*/true, GetUserData(ctx));
  }
};

// An SQL aggregate-function which performs a BFS from a given start node in a
// graph and returns all the nodes which are reachable from the start node.
//
// Note: this function is not intended to be used directly from SQL: instead
// macros exist in the standard library, wrapping it and making it
// user-friendly.
struct Bfs : public sqlite::AggregateFunction<Bfs> {
  static constexpr char kName[] = "__intrinsic_bfs";
  static constexpr int kArgCount = 2;
  using UserData = StringPool;

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    PERFETTO_DCHECK(argc == kArgCount);
    Search(ctx, argv, "BFS", &util::BreadthFirstSearch,
           /*reverse_start_ids=*/false, GetUserData(ctx));
  }
};

}  // namespace

namespace graph_traversal {
namespace {

class GraphTraversalPlugin : public Plugin<GraphTraversalPlugin> {
 public:
  ~GraphTraversalPlugin() override;

  void RegisterFunctions(PerfettoSqlConnection*,
                         std::vector<FunctionRegistration>& out) override {
    StringPool* pool = trace_context_->storage->mutable_string_pool();
    out.push_back(MakeFunctionRegistration<Dfs>(pool));
    out.push_back(MakeFunctionRegistration<Bfs>(pool));
  }
};

GraphTraversalPlugin::~GraphTraversalPlugin() = default;

}  // namespace

void RegisterPlugin() {
  static PluginRegistration reg(
      []() -> std::unique_ptr<PluginBase> {
        return std::make_unique<GraphTraversalPlugin>();
      },
      GraphTraversalPlugin::kPluginId, GraphTraversalPlugin::kDepIds.data(),
      GraphTraversalPlugin::kDepIds.size());
  base::ignore_result(reg);
}

}  // namespace graph_traversal
}  // namespace perfetto::trace_processor
