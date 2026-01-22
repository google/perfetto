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

#include "src/trace_processor/perfetto_sql/graph/graph_plugin.h"

#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/span.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/dataframe/dataframe.h"
#include "src/trace_processor/perfetto_sql/graph/graph.h"
#include "src/trace_processor/perfetto_sql/graph/graph_algorithms.h"
#include "src/trace_processor/perfetto_sql/intrinsic_helpers.h"
#include "src/trace_processor/perfetto_sql/tree/column_utils.h"
#include "src/trace_processor/perfetto_sql/tree/tree.h"
#include "src/trace_processor/plugins/plugin_context.h"
#include "src/trace_processor/sqlite/bindings/sqlite_aggregate_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"

namespace perfetto::trace_processor::plugins::graph {
namespace {

// Shared helpers from plugins namespace
using plugins::ExpectPointer;
using plugins::UniquePtrResult;

// Tree types (shared)
using plugins::tree::GetColumnTypes;
using plugins::tree::kNullInt64;
using plugins::tree::kNullUint32;
using plugins::tree::PushAllGatheredColumns;
using plugins::tree::PushSqliteValueToColumn;
using plugins::tree::Tree;

// =============================================================================
// __intrinsic_graph_nodes_agg - Aggregate to build node data
// =============================================================================

struct GraphNodesAggContext {
  StringPool* pool = nullptr;
  std::vector<int64_t> node_ids;
  std::vector<PassthroughColumn> passthrough_columns;
  int64_t min_id = std::numeric_limits<int64_t>::max();
  int64_t max_id = std::numeric_limits<int64_t>::min();
  bool first_row = true;
};

struct GraphNodesAgg : public sqlite::AggregateFunction<GraphNodesAgg> {
  using UserData = StringPool;
  static constexpr char kName[] = "__intrinsic_graph_nodes_agg";
  PERFETTO_TEMPLATED_USED static constexpr int kArgCount = -1;

  PERFETTO_TEMPLATED_USED static void Step(sqlite3_context* ctx,
                                           int argc,
                                           sqlite3_value** argv) {
    // argv[0] = node_id
    // argv[1..] = (col_name, col_value) pairs

    if (argc < 1 || (argc - 1) % 2 != 0) {
      return sqlite::utils::SetError(
          ctx,
          "__intrinsic_graph_nodes_agg: expected (id, [col_name, val]...)");
    }

    auto& agg_ctx = sqlite::AggregateContext<
        GraphNodesAggContext>::GetOrCreateContextForStep(ctx);

    constexpr uint32_t kInitialCapacity = 64 * 1024;

    if (agg_ctx.first_row) {
      agg_ctx.pool = GetUserData(ctx);
      agg_ctx.first_row = false;

      // Reserve capacity to reduce reallocations.
      agg_ctx.node_ids.reserve(kInitialCapacity);

      // Initialize passthrough columns.
      const uint32_t num_cols = static_cast<uint32_t>((argc - 1) / 2);
      agg_ctx.passthrough_columns.reserve(num_cols);
      for (int i = 1; i < argc; i += 2) {
        SQLITE_RETURN_IF_ERROR(
            ctx, sqlite::utils::ExpectArgType(argv[i], sqlite::Type::kText,
                                              kName, "column_name"));
        agg_ctx.passthrough_columns.emplace_back(sqlite::value::Text(argv[i]));
      }
    }

    // Get node_id.
    SQLITE_RETURN_IF_ERROR(
        ctx, sqlite::utils::ExpectArgType(argv[0], sqlite::Type::kInteger,
                                          kName, "id"));
    int64_t node_id = sqlite::value::Int64(argv[0]);

    // Track min/max for dense ID optimization.
    agg_ctx.min_id = std::min(agg_ctx.min_id, node_id);
    agg_ctx.max_id = std::max(agg_ctx.max_id, node_id);
    agg_ctx.node_ids.push_back(node_id);

    // Add column values.
    uint32_t col_idx = 0;
    for (int i = 2; i < argc; i += 2, ++col_idx) {
      if (col_idx >= agg_ctx.passthrough_columns.size()) {
        return sqlite::utils::SetError(
            ctx, "__intrinsic_graph_nodes_agg: column index out of bounds");
      }
      auto& col = agg_ctx.passthrough_columns[col_idx];

      if (PERFETTO_UNLIKELY(
              !PushSqliteValueToColumn(col, argv[i], agg_ctx.pool,
                                       kInitialCapacity))) {
        return sqlite::utils::SetError(
            ctx, "__intrinsic_graph_nodes_agg: type mismatch or blob value");
      }
    }
  }

  PERFETTO_TEMPLATED_USED static void Final(sqlite3_context* ctx) {
    auto agg_ctx = sqlite::AggregateContext<
        GraphNodesAggContext>::GetContextOrNullForFinal(ctx);

    if (!agg_ctx || agg_ctx.get()->node_ids.empty()) {
      return sqlite::result::Null(ctx);
    }

    auto* agg = agg_ctx.get();
    const auto num_nodes = static_cast<uint32_t>(agg->node_ids.size());

    // Build id_to_index map. Use dense vector if IDs are compact.
    const int64_t range = agg->max_id - agg->min_id + 1;
    const bool use_dense = range > 0 &&
                           static_cast<uint64_t>(range) / num_nodes < 4 &&
                           static_cast<uint64_t>(range) < 16 * 1024 * 1024;

    // Build GraphNodeData.
    auto nodes = std::make_unique<GraphNodeData>();

    if (use_dense) {
      // Use vector as direct-index map for dense IDs.
      nodes->id_to_index.min_id = agg->min_id;
      nodes->id_to_index.dense.resize(static_cast<size_t>(range), kNullUint32);
      for (uint32_t i = 0; i < num_nodes; ++i) {
        size_t idx = static_cast<size_t>(agg->node_ids[i] - agg->min_id);
        if (PERFETTO_UNLIKELY(nodes->id_to_index.dense[idx] != kNullUint32)) {
          return sqlite::utils::SetError(
              ctx,
              base::ErrStatus(
                  "__intrinsic_graph_nodes_agg: duplicate node_id: %" PRId64,
                  agg->node_ids[i]));
        }
        nodes->id_to_index.dense[idx] = i;
      }
    } else {
      // Use hashmap for sparse IDs.
      for (uint32_t i = 0; i < num_nodes; ++i) {
        auto res = nodes->id_to_index.sparse.Insert(agg->node_ids[i], i);
        if (PERFETTO_UNLIKELY(!res.second)) {
          return sqlite::utils::SetError(
              ctx,
              base::ErrStatus(
                  "__intrinsic_graph_nodes_agg: duplicate node_id: %" PRId64,
                  agg->node_ids[i]));
        }
      }
    }

    nodes->node_ids = std::move(agg->node_ids);
    nodes->passthrough_columns = std::move(agg->passthrough_columns);
    nodes->source_indices.resize(num_nodes);
    for (uint32_t i = 0; i < num_nodes; ++i) {
      nodes->source_indices[i] = i;
    }

    return UniquePtrResult(ctx, std::move(nodes));
  }
};

// =============================================================================
// __intrinsic_graph_edges_agg - Aggregate to build edge data
// =============================================================================

struct GraphEdgesAggContext {
  StringPool* pool = nullptr;
  std::vector<int64_t> source_ids;
  std::vector<int64_t> dest_ids;
  std::vector<PassthroughColumn> passthrough_columns;
  bool first_row = true;
};

struct GraphEdgesAgg : public sqlite::AggregateFunction<GraphEdgesAgg> {
  using UserData = StringPool;
  static constexpr char kName[] = "__intrinsic_graph_edges_agg";
  PERFETTO_TEMPLATED_USED static constexpr int kArgCount = -1;

  PERFETTO_TEMPLATED_USED static void Step(sqlite3_context* ctx,
                                           int argc,
                                           sqlite3_value** argv) {
    // argv[0] = source_id
    // argv[1] = dest_id
    // argv[2..] = (col_name, col_value) pairs

    if (argc < 2 || (argc - 2) % 2 != 0) {
      return sqlite::utils::SetError(
          ctx,
          "__intrinsic_graph_edges_agg: expected (source_id, dest_id, "
          "[col_name, val]...)");
    }

    auto& agg_ctx = sqlite::AggregateContext<
        GraphEdgesAggContext>::GetOrCreateContextForStep(ctx);

    constexpr uint32_t kInitialCapacity = 64 * 1024;

    if (agg_ctx.first_row) {
      agg_ctx.pool = GetUserData(ctx);
      agg_ctx.first_row = false;

      // Reserve capacity to reduce reallocations.
      agg_ctx.source_ids.reserve(kInitialCapacity);
      agg_ctx.dest_ids.reserve(kInitialCapacity);

      // Initialize passthrough columns.
      const uint32_t num_cols = static_cast<uint32_t>((argc - 2) / 2);
      agg_ctx.passthrough_columns.reserve(num_cols);
      for (int i = 2; i < argc; i += 2) {
        SQLITE_RETURN_IF_ERROR(
            ctx, sqlite::utils::ExpectArgType(argv[i], sqlite::Type::kText,
                                              kName, "column_name"));
        agg_ctx.passthrough_columns.emplace_back(sqlite::value::Text(argv[i]));
      }
    }

    // Get source_id.
    SQLITE_RETURN_IF_ERROR(
        ctx, sqlite::utils::ExpectArgType(argv[0], sqlite::Type::kInteger,
                                          kName, "source_id"));
    int64_t source_id = sqlite::value::Int64(argv[0]);

    // Get dest_id.
    SQLITE_RETURN_IF_ERROR(
        ctx, sqlite::utils::ExpectArgType(argv[1], sqlite::Type::kInteger,
                                          kName, "dest_id"));
    int64_t dest_id = sqlite::value::Int64(argv[1]);

    agg_ctx.source_ids.push_back(source_id);
    agg_ctx.dest_ids.push_back(dest_id);

    // Add column values.
    uint32_t col_idx = 0;
    for (int i = 3; i < argc; i += 2, ++col_idx) {
      if (col_idx >= agg_ctx.passthrough_columns.size()) {
        return sqlite::utils::SetError(
            ctx, "__intrinsic_graph_edges_agg: column index out of bounds");
      }
      auto& col = agg_ctx.passthrough_columns[col_idx];

      if (PERFETTO_UNLIKELY(
              !PushSqliteValueToColumn(col, argv[i], agg_ctx.pool,
                                       kInitialCapacity))) {
        return sqlite::utils::SetError(
            ctx, "__intrinsic_graph_edges_agg: type mismatch or blob value");
      }
    }
  }

  PERFETTO_TEMPLATED_USED static void Final(sqlite3_context* ctx) {
    auto agg_ctx = sqlite::AggregateContext<
        GraphEdgesAggContext>::GetContextOrNullForFinal(ctx);

    if (!agg_ctx) {
      return sqlite::result::Null(ctx);
    }

    auto* agg = agg_ctx.get();

    // Build GraphEdgeData (without node index resolution yet).
    auto edges = std::make_unique<GraphEdgeData>();
    const auto num_edges = static_cast<uint32_t>(agg->source_ids.size());

    // Store source/dest IDs temporarily in
    // source_node_indices/dest_node_indices as int64 values. They'll be
    // resolved to indices in __intrinsic_graph_build. This is a bit of a hack -
    // we store IDs, not indices, and resolve later.
    edges->passthrough_columns = std::move(agg->passthrough_columns);
    edges->source_indices.resize(num_edges);
    for (uint32_t i = 0; i < num_edges; ++i) {
      edges->source_indices[i] = i;
    }

    // Store source/dest IDs as passthrough columns for later resolution.
    edges->passthrough_columns.insert(
        edges->passthrough_columns.begin(),
        PassthroughColumn("__source_id", std::move(agg->source_ids)));
    edges->passthrough_columns.insert(
        edges->passthrough_columns.begin() + 1,
        PassthroughColumn("__dest_id", std::move(agg->dest_ids)));

    return UniquePtrResult(ctx, std::move(edges));
  }
};

// =============================================================================
// __intrinsic_graph_build(nodes, edges) -> GRAPH
// =============================================================================
struct GraphBuild : public sqlite::Function<GraphBuild> {
  static constexpr char kName[] = "__intrinsic_graph_build";
  PERFETTO_TEMPLATED_USED static constexpr int kArgCount = 2;

  PERFETTO_TEMPLATED_USED static void Step(sqlite3_context* ctx,
                                           int argc,
                                           sqlite3_value** argv) {
    SQLITE_RETURN_IF_ERROR(
        ctx, sqlite::utils::CheckArgCount(kName, static_cast<size_t>(argc), 2));
    SQLITE_ASSIGN_OR_RETURN(ctx, auto* nodes,
                            ExpectPointer<GraphNodeData>(argv[0], kName));
    SQLITE_ASSIGN_OR_RETURN(ctx, auto* edges,
                            ExpectPointer<GraphEdgeData>(argv[1], kName));

    // Build the Graph.
    auto graph = std::make_unique<Graph>();
    graph->data = std::make_shared<GraphData>();

    // Move node data.
    graph->data->nodes.node_ids = std::move(nodes->node_ids);
    graph->data->nodes.source_indices = std::move(nodes->source_indices);
    graph->data->nodes.passthrough_columns = std::move(nodes->passthrough_columns);
    graph->data->nodes.id_to_index = std::move(nodes->id_to_index);

    // Resolve edge source/dest IDs to node indices.
    // The first two passthrough columns are __source_id and __dest_id.
    if (edges->passthrough_columns.size() < 2 ||
        edges->passthrough_columns[0].name != "__source_id" ||
        edges->passthrough_columns[1].name != "__dest_id") {
      return sqlite::utils::SetError(
          ctx, "__intrinsic_graph_build: edges missing __source_id/__dest_id");
    }

    const auto& src_ids = edges->passthrough_columns[0].AsInt64();
    const auto& dst_ids = edges->passthrough_columns[1].AsInt64();
    const uint32_t num_edges = static_cast<uint32_t>(src_ids.size());

    graph->data->edges.source_node_indices.resize(num_edges);
    graph->data->edges.dest_node_indices.resize(num_edges);
    graph->data->edges.source_indices.resize(num_edges);

    for (uint32_t i = 0; i < num_edges; ++i) {
      int64_t src_id = src_ids[i];
      int64_t dst_id = dst_ids[i];

      auto* src_ptr = graph->data->nodes.id_to_index.Find(src_id);
      auto* dst_ptr = graph->data->nodes.id_to_index.Find(dst_id);

      if (!src_ptr) {
        return sqlite::utils::SetError(ctx,
                                       "Edge source_id not found in nodes");
      }
      if (!dst_ptr) {
        return sqlite::utils::SetError(ctx, "Edge dest_id not found in nodes");
      }

      graph->data->edges.source_node_indices[i] = *src_ptr;
      graph->data->edges.dest_node_indices[i] = *dst_ptr;
      graph->data->edges.source_indices[i] = i;
    }

    // Copy edge passthrough columns (skip __source_id and __dest_id).
    for (size_t i = 2; i < edges->passthrough_columns.size(); ++i) {
      graph->data->edges.passthrough_columns.push_back(
          edges->passthrough_columns[i]);
    }

    return UniquePtrResult(ctx, std::move(graph));
  }
};

// =============================================================================
// __intrinsic_graph_filter(graph, column_name) -> GRAPH
// =============================================================================
struct GraphFilter : public sqlite::Function<GraphFilter> {
  static constexpr char kName[] = "__intrinsic_graph_filter";
  PERFETTO_TEMPLATED_USED static constexpr int kArgCount = 2;

  PERFETTO_TEMPLATED_USED static void Step(sqlite3_context* ctx,
                                           int argc,
                                           sqlite3_value** argv) {
    SQLITE_RETURN_IF_ERROR(
        ctx, sqlite::utils::CheckArgCount(kName, static_cast<size_t>(argc), 2));
    SQLITE_ASSIGN_OR_RETURN(ctx, auto* graph,
                            ExpectPointer<Graph>(argv[0], kName));
    SQLITE_RETURN_IF_ERROR(
        ctx, sqlite::utils::ExpectArgType(argv[1], sqlite::Type::kText, kName,
                                          "column_name"));
    std::string column_name = sqlite::value::Text(argv[1]);

    return UniquePtrResult(
        ctx, graph->CopyAndAddOp(GraphFilterOp(std::move(column_name))));
  }
};

// =============================================================================
// RootIds - opaque type for collected root IDs
// =============================================================================

struct RootIds {
  static constexpr const char* kPointerType = "ROOT_IDS";
  std::vector<int64_t> ids;
};

// =============================================================================
// __intrinsic_graph_roots_agg(root_id) -> ROOT_IDS
// Aggregate function that collects root IDs into a pointer.
// =============================================================================

struct GraphRootsAggContext {
  std::vector<int64_t> root_ids;
};

struct GraphRootsAgg : public sqlite::AggregateFunction<GraphRootsAgg> {
  static constexpr char kName[] = "__intrinsic_graph_roots_agg";
  PERFETTO_TEMPLATED_USED static constexpr int kArgCount = 1;

  PERFETTO_TEMPLATED_USED static void Step(sqlite3_context* ctx,
                                           int argc,
                                           sqlite3_value** argv) {
    SQLITE_RETURN_IF_ERROR(
        ctx, sqlite::utils::CheckArgCount(kName, static_cast<size_t>(argc), 1));

    auto& agg_ctx =
        sqlite::AggregateContext<GraphRootsAggContext>::GetOrCreateContextForStep(
            ctx);

    // Collect root IDs (skip nulls).
    if (sqlite::value::Type(argv[0]) == sqlite::Type::kInteger) {
      agg_ctx.root_ids.push_back(sqlite::value::Int64(argv[0]));
    }
  }

  PERFETTO_TEMPLATED_USED static void Final(sqlite3_context* ctx) {
    auto agg_ctx = sqlite::AggregateContext<
        GraphRootsAggContext>::GetContextOrNullForFinal(ctx);

    if (!agg_ctx) {
      return sqlite::result::Null(ctx);
    }

    auto roots = std::make_unique<RootIds>();
    roots->ids = std::move(agg_ctx.get()->root_ids);
    return UniquePtrResult(ctx, std::move(roots));
  }
};

// =============================================================================
// __intrinsic_graph_to_tree(graph, roots, mode) -> TREE
// Scalar function that converts a graph to a tree.
// =============================================================================

struct GraphToTree_Fn : public sqlite::Function<GraphToTree_Fn> {
  static constexpr char kName[] = "__intrinsic_graph_to_tree";
  PERFETTO_TEMPLATED_USED static constexpr int kArgCount = 3;

  PERFETTO_TEMPLATED_USED static void Step(sqlite3_context* ctx,
                                           int argc,
                                           sqlite3_value** argv) {
    SQLITE_RETURN_IF_ERROR(
        ctx, sqlite::utils::CheckArgCount(kName, static_cast<size_t>(argc), 3));

    SQLITE_ASSIGN_OR_RETURN(ctx, auto* graph,
                            ExpectPointer<Graph>(argv[0], kName));
    SQLITE_ASSIGN_OR_RETURN(ctx, auto* roots,
                            ExpectPointer<RootIds>(argv[1], kName));
    SQLITE_RETURN_IF_ERROR(ctx, sqlite::utils::ExpectArgType(
                                    argv[2], sqlite::Type::kText, kName, "mode"));

    std::string mode_str = sqlite::value::Text(argv[2]);
    GraphTraversalMode mode;
    if (mode_str == "BFS") {
      mode = GraphTraversalMode::kBfs;
    } else if (mode_str == "DFS") {
      mode = GraphTraversalMode::kDfs;
    } else {
      return sqlite::utils::SetError(
          ctx, "__intrinsic_graph_to_tree: mode must be BFS or DFS");
    }

    GraphData& data = *graph->data;

    // Apply pending filter operations to edges.
    GraphEdgeData working_edges = data.edges;
    for (const auto& op_variant : graph->pending_ops) {
      if (const auto* filter_op = std::get_if<GraphFilterOp>(&op_variant)) {
        SQLITE_ASSIGN_OR_RETURN(
            ctx, auto filter_result,
            FilterEdges(working_edges, filter_op->column_name));
        working_edges = std::move(filter_result.new_edges);
      }
    }

    // Convert root IDs to indices.
    std::vector<uint32_t> root_indices;
    for (int64_t root_id : roots->ids) {
      auto* idx_ptr = data.nodes.id_to_index.Find(root_id);
      if (idx_ptr) {
        root_indices.push_back(*idx_ptr);
      }
    }

    // Run graph to tree conversion (moves nodes, invalidating graph).
    SQLITE_ASSIGN_OR_RETURN(
        ctx, auto result,
        GraphToTree(std::move(data.nodes), working_edges, root_indices, mode));
    return UniquePtrResult(ctx, std::move(result.tree));
  }
};

// =============================================================================
// __intrinsic_graph_node_emit(graph) -> TABLE
// =============================================================================
struct GraphNodeEmit : public sqlite::Function<GraphNodeEmit> {
  using UserData = StringPool;
  static constexpr char kName[] = "__intrinsic_graph_node_emit";
  PERFETTO_TEMPLATED_USED static constexpr int kArgCount = 1;

  PERFETTO_TEMPLATED_USED static void Step(sqlite3_context* ctx,
                                           int argc,
                                           sqlite3_value** argv) {
    SQLITE_RETURN_IF_ERROR(
        ctx, sqlite::utils::CheckArgCount(kName, static_cast<size_t>(argc), 1));
    SQLITE_ASSIGN_OR_RETURN(ctx, auto* graph,
                            ExpectPointer<Graph>(argv[0], kName));
    StringPool* pool = GetUserData(ctx);
    const auto& nodes = graph->data->nodes;

    // Build column names: id + passthrough columns.
    std::vector<std::string> column_names;
    column_names.emplace_back("id");
    for (const auto& col : nodes.passthrough_columns) {
      column_names.push_back(col.name);
    }

    // Build column types.
    using ColType = dataframe::AdhocDataframeBuilder::ColumnType;
    std::vector<ColType> col_types;
    col_types.push_back(ColType::kInt64);  // id
    auto pt_col_types = GetColumnTypes(nodes.passthrough_columns);
    col_types.insert(col_types.end(), pt_col_types.begin(), pt_col_types.end());

    dataframe::AdhocDataframeBuilder builder(column_names, pool, col_types);

    uint32_t col = 0;

    // id column.
    builder.PushSpanUnchecked(col++, base::MakeSpan(nodes.node_ids));

    // Passthrough columns via source_indices.
    PushAllGatheredColumns(builder, col, nodes.passthrough_columns,
                           base::MakeSpan(nodes.source_indices));

    SQLITE_ASSIGN_OR_RETURN(ctx, auto df, std::move(builder).Build());
    return sqlite::result::UniquePointer(
        ctx, std::make_unique<dataframe::Dataframe>(std::move(df)), "TABLE");
  }
};

// =============================================================================
// __intrinsic_graph_edge_emit(graph) -> TABLE
// =============================================================================
struct GraphEdgeEmit : public sqlite::Function<GraphEdgeEmit> {
  using UserData = StringPool;
  static constexpr char kName[] = "__intrinsic_graph_edge_emit";
  PERFETTO_TEMPLATED_USED static constexpr int kArgCount = 1;

  PERFETTO_TEMPLATED_USED static void Step(sqlite3_context* ctx,
                                           int argc,
                                           sqlite3_value** argv) {
    SQLITE_RETURN_IF_ERROR(
        ctx, sqlite::utils::CheckArgCount(kName, static_cast<size_t>(argc), 1));
    SQLITE_ASSIGN_OR_RETURN(ctx, auto* graph,
                            ExpectPointer<Graph>(argv[0], kName));
    StringPool* pool = GetUserData(ctx);
    const GraphData& data = *graph->data;

    // Apply pending filter operations to edges.
    GraphEdgeData working_edges = data.edges;
    for (const auto& op_variant : graph->pending_ops) {
      if (const auto* filter_op = std::get_if<GraphFilterOp>(&op_variant)) {
        SQLITE_ASSIGN_OR_RETURN(
            ctx, auto filter_result,
            FilterEdges(working_edges, filter_op->column_name));
        working_edges = std::move(filter_result.new_edges);
      }
    }

    const auto& edges = working_edges;
    const auto& nodes = data.nodes;

    // Build column names: source_id, dest_id + passthrough columns.
    std::vector<std::string> column_names;
    column_names.emplace_back("source_id");
    column_names.emplace_back("dest_id");
    for (const auto& col : edges.passthrough_columns) {
      column_names.push_back(col.name);
    }

    // Build column types.
    using ColType = dataframe::AdhocDataframeBuilder::ColumnType;
    std::vector<ColType> col_types;
    col_types.push_back(ColType::kInt64);  // source_id
    col_types.push_back(ColType::kInt64);  // dest_id
    auto pt_col_types = GetColumnTypes(edges.passthrough_columns);
    col_types.insert(col_types.end(), pt_col_types.begin(), pt_col_types.end());

    dataframe::AdhocDataframeBuilder builder(column_names, pool, col_types);

    uint32_t col = 0;

    // source_id column (look up original ID from node index).
    builder.PushGatheredUnchecked(col++, base::MakeSpan(nodes.node_ids),
                                  base::MakeSpan(edges.source_node_indices));

    // dest_id column.
    builder.PushGatheredUnchecked(col++, base::MakeSpan(nodes.node_ids),
                                  base::MakeSpan(edges.dest_node_indices));

    // Passthrough columns via source_indices.
    PushAllGatheredColumns(builder, col, edges.passthrough_columns,
                           base::MakeSpan(edges.source_indices));

    SQLITE_ASSIGN_OR_RETURN(ctx, auto df, std::move(builder).Build());
    return sqlite::result::UniquePointer(
        ctx, std::make_unique<dataframe::Dataframe>(std::move(df)), "TABLE");
  }
};

}  // namespace

base::Status GraphPlugin::Register(PluginContext& ctx) {
  // Graph construction aggregates.
  RETURN_IF_ERROR(ctx.RegisterAggregateFunction<GraphNodesAgg>(ctx.pool()));
  RETURN_IF_ERROR(ctx.RegisterAggregateFunction<GraphEdgesAgg>(ctx.pool()));
  RETURN_IF_ERROR(ctx.RegisterFunction<GraphBuild>(nullptr));

  // Graph operation functions.
  RETURN_IF_ERROR(ctx.RegisterFunction<GraphFilter>(nullptr));

  // Graph to tree conversion: roots aggregate + scalar function.
  RETURN_IF_ERROR(ctx.RegisterAggregateFunction<GraphRootsAgg>(nullptr));
  RETURN_IF_ERROR(ctx.RegisterFunction<GraphToTree_Fn>(nullptr));

  // Graph emit functions (need pool for building output dataframe).
  RETURN_IF_ERROR(ctx.RegisterFunction<GraphNodeEmit>(ctx.pool()));
  RETURN_IF_ERROR(ctx.RegisterFunction<GraphEdgeEmit>(ctx.pool()));

  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::plugins::graph
