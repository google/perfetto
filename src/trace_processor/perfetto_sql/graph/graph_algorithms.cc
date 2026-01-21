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

#include "src/trace_processor/perfetto_sql/graph/graph_algorithms.h"

#include <cstdint>
#include <memory>
#include <queue>
#include <stack>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/perfetto_sql/graph/graph.h"
#include "src/trace_processor/perfetto_sql/tree/column_utils.h"
#include "src/trace_processor/perfetto_sql/tree/tree.h"

namespace perfetto::trace_processor::plugins::graph {

// Import types from tree namespace that we use
using plugins::tree::FindColumnByName;
using plugins::tree::FindColumnOrError;
using plugins::tree::GatherAllPassthroughColumns;
using plugins::tree::kNullInt64;
using plugins::tree::kNullUint32;
using plugins::tree::PassthroughColumn;
using plugins::tree::Tree;
using plugins::tree::TreeData;

std::vector<std::vector<uint32_t>> BuildAdjacencyList(
    const GraphEdgeData& edges,
    uint32_t num_nodes) {
  std::vector<std::vector<uint32_t>> adj(num_nodes);
  const uint32_t num_edges =
      static_cast<uint32_t>(edges.source_node_indices.size());
  for (uint32_t i = 0; i < num_edges; ++i) {
    uint32_t src = edges.source_node_indices[i];
    uint32_t dst = edges.dest_node_indices[i];
    if (src < num_nodes && dst < num_nodes) {
      adj[src].push_back(dst);
    }
  }
  return adj;
}

base::StatusOr<GraphFilterResult> FilterEdges(const GraphEdgeData& edges,
                                              const std::string& column_name) {
  const auto* col = FindColumnByName(edges.passthrough_columns, column_name);
  if (!col) {
    return base::ErrStatus("FilterEdges: column '%s' not found",
                           column_name.c_str());
  }

  if (!col->IsInt64()) {
    return base::ErrStatus("FilterEdges: column '%s' must be integer (boolean)",
                           column_name.c_str());
  }

  const auto& filter_values = col->AsInt64();
  const auto num_edges = static_cast<uint32_t>(edges.source_indices.size());

  GraphFilterResult result;

  // Count non-filtered edges first for reservation.
  uint32_t kept_count = 0;
  for (uint32_t i = 0; i < num_edges; ++i) {
    uint32_t src_idx = edges.source_indices[i];
    int64_t filter_val = filter_values[src_idx];
    // Keep edge if filter value is 0 (false) or null.
    if (filter_val == 0 || filter_val == kNullInt64) {
      ++kept_count;
    }
  }

  result.new_edges.source_node_indices.reserve(kept_count);
  result.new_edges.dest_node_indices.reserve(kept_count);
  result.new_edges.source_indices.reserve(kept_count);

  // Copy non-filtered edges.
  for (uint32_t i = 0; i < num_edges; ++i) {
    uint32_t src_idx = edges.source_indices[i];
    int64_t filter_val = filter_values[src_idx];
    // Keep edge if filter value is 0 (false) or null.
    if (filter_val == 0 || filter_val == kNullInt64) {
      result.new_edges.source_node_indices.push_back(
          edges.source_node_indices[i]);
      result.new_edges.dest_node_indices.push_back(edges.dest_node_indices[i]);
      result.new_edges.source_indices.push_back(src_idx);
    }
  }

  // Copy passthrough columns (still accessed via source_indices).
  result.new_edges.passthrough_columns = edges.passthrough_columns;

  return result;
}

base::StatusOr<GraphToTreeResult> GraphToTree(
    const GraphNodeData& nodes,
    const GraphEdgeData& edges,
    const std::vector<uint32_t>& root_node_indices,
    GraphTraversalMode mode) {
  const auto num_nodes = static_cast<uint32_t>(nodes.node_ids.size());

  if (num_nodes == 0) {
    return base::ErrStatus("GraphToTree: graph has no nodes");
  }

  // Build adjacency list for traversal.
  auto adj = BuildAdjacencyList(edges, num_nodes);

  // Track visited nodes and their parents in the traversal tree.
  std::vector<bool> visited(num_nodes, false);
  std::vector<uint32_t> parent(num_nodes, kNullUint32);

  // Output: nodes in traversal order.
  std::vector<uint32_t> traversal_order;
  traversal_order.reserve(num_nodes);

  if (mode == GraphTraversalMode::kBfs) {
    // BFS traversal.
    std::queue<uint32_t> queue;
    for (uint32_t root : root_node_indices) {
      if (root < num_nodes && !visited[root]) {
        visited[root] = true;
        parent[root] = kNullUint32;  // Root has no parent.
        queue.push(root);
        traversal_order.push_back(root);
      }
    }

    while (!queue.empty()) {
      uint32_t curr = queue.front();
      queue.pop();

      for (uint32_t neighbor : adj[curr]) {
        if (!visited[neighbor]) {
          visited[neighbor] = true;
          parent[neighbor] = curr;
          queue.push(neighbor);
          traversal_order.push_back(neighbor);
        }
      }
    }
  } else {
    // DFS traversal.
    std::stack<uint32_t> stack;
    for (uint32_t root : root_node_indices) {
      if (root < num_nodes && !visited[root]) {
        visited[root] = true;
        parent[root] = kNullUint32;
        stack.push(root);
        traversal_order.push_back(root);
      }
    }

    while (!stack.empty()) {
      uint32_t curr = stack.top();
      stack.pop();

      // Process children in reverse order so they're visited in forward order.
      for (auto it = adj[curr].rbegin(); it != adj[curr].rend(); ++it) {
        uint32_t neighbor = *it;
        if (!visited[neighbor]) {
          visited[neighbor] = true;
          parent[neighbor] = curr;
          stack.push(neighbor);
          traversal_order.push_back(neighbor);
        }
      }
    }
  }

  // Build the Tree result.
  const uint32_t tree_size = static_cast<uint32_t>(traversal_order.size());

  auto tree = std::make_unique<Tree>();
  tree->data = std::make_shared<TreeData>();

  // Map from original node index to tree index.
  std::vector<uint32_t> node_to_tree(num_nodes, kNullUint32);
  for (uint32_t tree_idx = 0; tree_idx < tree_size; ++tree_idx) {
    node_to_tree[traversal_order[tree_idx]] = tree_idx;
  }

  // Build parent_indices for the tree.
  tree->data->parent_indices.resize(tree_size);
  for (uint32_t tree_idx = 0; tree_idx < tree_size; ++tree_idx) {
    uint32_t node_idx = traversal_order[tree_idx];
    uint32_t parent_node_idx = parent[node_idx];
    if (parent_node_idx == kNullUint32) {
      tree->data->parent_indices[tree_idx] = kNullUint32;
    } else {
      tree->data->parent_indices[tree_idx] = node_to_tree[parent_node_idx];
    }
  }

  // Build source_indices for passthrough columns.
  // Maps tree index -> original node source index.
  tree->data->source_indices.resize(tree_size);
  for (uint32_t tree_idx = 0; tree_idx < tree_size; ++tree_idx) {
    uint32_t node_idx = traversal_order[tree_idx];
    tree->data->source_indices[tree_idx] = nodes.source_indices[node_idx];
  }

  // Add original_id and original_parent_id columns.
  std::vector<int64_t> id_values(tree_size);
  for (uint32_t tree_idx = 0; tree_idx < tree_size; ++tree_idx) {
    id_values[tree_idx] = nodes.node_ids[traversal_order[tree_idx]];
  }
  tree->data->passthrough_columns.emplace_back(Tree::kOriginalIdCol,
                                               std::move(id_values));

  std::vector<int64_t> parent_id_values(tree_size);
  for (uint32_t tree_idx = 0; tree_idx < tree_size; ++tree_idx) {
    uint32_t parent_node_idx = parent[traversal_order[tree_idx]];
    parent_id_values[tree_idx] = parent_node_idx == kNullUint32
                                     ? kNullInt64
                                     : nodes.node_ids[parent_node_idx];
  }
  tree->data->passthrough_columns.emplace_back(Tree::kOriginalParentIdCol,
                                               std::move(parent_id_values));

  // Copy node passthrough columns.
  for (const auto& col : nodes.passthrough_columns) {
    tree->data->passthrough_columns.push_back(col);
  }

  return GraphToTreeResult(std::move(tree));
}

}  // namespace perfetto::trace_processor::plugins::graph
