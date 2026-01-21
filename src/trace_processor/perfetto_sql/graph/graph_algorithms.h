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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_GRAPH_GRAPH_ALGORITHMS_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_GRAPH_GRAPH_ALGORITHMS_H_

#include <cstdint>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/perfetto_sql/graph/graph.h"
#include "src/trace_processor/perfetto_sql/tree/tree.h"

namespace perfetto::trace_processor::plugins::graph {

// Import Tree type from tree namespace
using plugins::tree::Tree;

// Traversal mode for graph to tree conversion.
enum class GraphTraversalMode {
  kBfs,  // Breadth-first search (shortest path tree)
  kDfs,  // Depth-first search
};

// Result of filtering edges.
struct GraphFilterResult {
  // New edge data after filtering.
  GraphEdgeData new_edges;
};

// Filters edges where the specified boolean column is true.
// Removes edges where column value is truthy (non-zero for int64).
//
// Args:
//   edges: The edge data to filter
//   column_name: Name of boolean column in edge passthrough columns
//
// Returns new edge data with filtered edges removed.
base::StatusOr<GraphFilterResult> FilterEdges(const GraphEdgeData& edges,
                                              const std::string& column_name);

// Result of converting graph to tree.
struct GraphToTreeResult {
  explicit GraphToTreeResult(std::unique_ptr<Tree> t) : tree(std::move(t)) {}
  std::unique_ptr<Tree> tree;
};

// Converts a graph to a tree using BFS or DFS from root nodes.
//
// For BFS: produces shortest-path tree (each node's parent is the node
// that discovered it first in BFS order).
//
// For DFS: produces DFS tree (each node's parent is the node that
// discovered it first in DFS order).
//
// Only nodes reachable from roots are included in the output tree.
// Node passthrough columns are carried through to the tree.
//
// Args:
//   nodes: Node data (IDs, passthrough columns)
//   edges: Edge data (after any pending filter ops applied)
//   root_node_indices: Internal indices of root nodes to start traversal
//   mode: BFS or DFS traversal mode
//
// Returns a Tree with the traversal result.
base::StatusOr<GraphToTreeResult> GraphToTree(
    GraphNodeData nodes,
    const GraphEdgeData& edges,
    const std::vector<uint32_t>& root_node_indices,
    GraphTraversalMode mode);

// Builds adjacency list from edge data for efficient traversal.
// Returns CSR where adj[i] contains indices of nodes reachable from node i.
plugins::tree::CsrVector<uint32_t> BuildAdjacencyList(const GraphEdgeData& edges,
                                                      uint32_t num_nodes);

}  // namespace perfetto::trace_processor::plugins::graph

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_GRAPH_GRAPH_ALGORITHMS_H_
