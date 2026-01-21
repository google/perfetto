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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_GRAPH_GRAPH_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_GRAPH_GRAPH_H_

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/perfetto_sql/tree/tree.h"  // For PassthroughColumn

namespace perfetto::trace_processor::plugins::graph {

// Import types from tree namespace that are shared
using plugins::tree::PassthroughColumn;

// Operation: filter edges where a boolean column is true.
struct GraphFilterOp {
  explicit GraphFilterOp(std::string col) : column_name(std::move(col)) {}
  std::string column_name;  // Boolean edge column to filter on
};

// All possible graph operations.
using GraphOp = std::variant<GraphFilterOp>;

// Inner data storage for edge table.
struct GraphEdgeData {
  static constexpr const char* kPointerType = "GRAPH_EDGES";
  // For each edge: index into node table for source node.
  std::vector<uint32_t> source_node_indices;
  // For each edge: index into node table for destination node.
  std::vector<uint32_t> dest_node_indices;

  // Index into edge passthrough columns for each edge.
  // Allows lazy access: filter ops compact this without touching columns.
  std::vector<uint32_t> source_indices;

  // Passthrough edge columns. Accessed via source_indices indirection.
  std::vector<PassthroughColumn> passthrough_columns;
};

// Inner data storage for node table.
struct GraphNodeData {
  static constexpr const char* kPointerType = "GRAPH_NODES";
  // Original node IDs from input.
  std::vector<int64_t> node_ids;

  // Index into node passthrough columns for each node.
  // Allows lazy access: operations can compact without touching columns.
  std::vector<uint32_t> source_indices;

  // Passthrough node columns. Accessed via source_indices indirection.
  std::vector<PassthroughColumn> passthrough_columns;

  // Map from original node ID to internal index.
  base::FlatHashMapV2<int64_t, uint32_t> id_to_index;
};

// Inner data storage for Graph, wrapped in shared_ptr for cheap copying.
struct GraphData {
  GraphEdgeData edges;
  GraphNodeData nodes;
};

// The GRAPH opaque type.
//
// Stores graph structure with separate edge and node tables.
// Operations are lazy - queued in pending_ops and executed on emit.
//
// Data is wrapped in shared_ptr for cheap copying when adding lazy operations.
struct Graph {
  static constexpr const char* kPointerType = "GRAPH";

  // Shared data storage (cheap to copy via shared_ptr).
  std::shared_ptr<GraphData> data;

  // Pending operations to apply at emit time.
  std::vector<GraphOp> pending_ops;

  Graph() = default;
  Graph(std::shared_ptr<GraphData> d, std::vector<GraphOp> ops)
      : data(std::move(d)), pending_ops(std::move(ops)) {}

  // Create a copy sharing the same data (for adding lazy operations).
  std::unique_ptr<Graph> Copy() const {
    return std::make_unique<Graph>(data, pending_ops);
  }

  // Create a copy and add an operation in one step.
  std::unique_ptr<Graph> CopyAndAddOp(GraphOp op) const {
    auto copy = Copy();
    copy->pending_ops.push_back(std::move(op));
    return copy;
  }
};

}  // namespace perfetto::trace_processor::plugins::graph

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_GRAPH_GRAPH_H_
