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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_TREE_TREE_ALGORITHMS_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_TREE_TREE_ALGORITHMS_H_

#include <cstdint>
#include <utility>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/perfetto_sql/tree/tree.h"

namespace perfetto::trace_processor::plugins::tree {

// Input row for building a tree from parent references.
struct TreeInputRow {
  int64_t id;
  int64_t parent_id;  // kNullInt64 for roots
};

// Result of building a tree: maps original IDs to internal row indices.
struct TreeBuildResult {
  // For each row index, the node_id
  std::vector<int64_t> node_ids;
  // For each row index, the parent's row index (kNullUint32 for roots)
  std::vector<uint32_t> parent_indices;
};

// Validates and builds the tree structure from parent references.
// Returns the mapping from node IDs to row indices and parent relationships.
//
// Checks for:
// - Duplicate node IDs
// - (Future: cycles, orphans)
base::StatusOr<TreeBuildResult> BuildTreeStructure(
    const std::vector<TreeInputRow>& rows);

// Computes depth for each node given parent indices.
// Root nodes (parent_idx == kNullUint32) have depth 0.
std::vector<uint32_t> ComputeDepths(
    const std::vector<uint32_t>& parent_indices);

// Returns node indices in topological order (roots first, then children).
// This is a BFS traversal from roots to leaves.
// Useful for operations that need to process parents before children.
std::vector<uint32_t> TopologicalOrder(
    const std::vector<uint32_t>& parent_indices);

// Returns node indices in reverse topological order (leaves first, then
// parents). This is useful for bottom-up aggregations where children must be
// processed before their parents.

// Represents a node and its children for merge operations.
struct TreeNode {
  uint32_t row_idx;
  std::vector<uint32_t> children;  // Row indices of children
};

// Builds a map from row index to its children's row indices.
// Returns a CsrVector where children of node i are at indices
// [offsets[i], offsets[i+1]).
CsrVector<uint32_t> BuildChildrenMap(
    const std::vector<uint32_t>& parent_indices);

// Result of merging siblings.
struct MergeSiblingsResult {
  // For each output row, the source row indices that were merged into it.
  // Sources for output row i are at merged_sources[offsets[i]..offsets[i+1]).
  CsrVector<uint32_t> merged_sources;
  // New parent indices after merging (kNullUint32 for roots).
  std::vector<uint32_t> new_parent_indices;
  // Mapping from old row index to new row index (kNullUint32 if merged away).
  std::vector<uint32_t> old_to_new;
};

// Merges sibling nodes that share the same key value.
//
// Args:
//   parent_indices: For each row, the parent's row index (kNullUint32 for
//   roots) key_values: The key column values used for grouping siblings
//   order_values: The order column values for determining sibling order
//   mode: CONSECUTIVE merges only adjacent siblings, GLOBAL merges all
//
// Returns mapping information for applying aggregations.
template <typename KeyT, typename OrderT>
base::StatusOr<MergeSiblingsResult> MergeSiblings(
    const std::vector<uint32_t>& parent_indices,
    const std::vector<KeyT>& key_values,
    const std::vector<OrderT>& order_values,
    TreeMergeMode mode);

// Applies an aggregation to merge source values into a single output value.
template <typename T>
T ApplyAggregation(const std::vector<T>& values, TreeAggType agg_type);

// Specializations declared here, defined in .cc
template <>
int64_t ApplyAggregation(const std::vector<int64_t>& values,
                         TreeAggType agg_type);

template <>
double ApplyAggregation(const std::vector<double>& values,
                        TreeAggType agg_type);

// Aggregates a column of values according to merge sources.
// For each output row, collects values from source rows and applies
// aggregation.
template <typename T>
std::vector<T> AggregateColumn(const std::vector<T>& src_values,
                               const CsrVector<uint32_t>& merged_sources,
                               TreeAggType agg_type) {
  std::vector<T> result;
  result.reserve(merged_sources.size());
  for (auto sources : merged_sources) {
    if (agg_type == TreeAggType::kAny || sources.size() == 1) {
      result.push_back(src_values[sources[0]]);
    } else {
      std::vector<T> vals;
      vals.reserve(sources.size());
      for (uint32_t src : sources) {
        vals.push_back(src_values[src]);
      }
      result.push_back(ApplyAggregation<T>(vals, agg_type));
    }
  }
  return result;
}

// Explicit instantiation declarations for MergeSiblings.
// Definitions are in tree_algorithms.cc.
extern template base::StatusOr<MergeSiblingsResult>
MergeSiblings<int64_t, int64_t>(const std::vector<uint32_t>& parent_indices,
                                const std::vector<int64_t>& key_values,
                                const std::vector<int64_t>& order_values,
                                TreeMergeMode mode);

extern template base::StatusOr<MergeSiblingsResult>
MergeSiblings<StringPool::Id, int64_t>(
    const std::vector<uint32_t>& parent_indices,
    const std::vector<StringPool::Id>& key_values,
    const std::vector<int64_t>& order_values,
    TreeMergeMode mode);

// Result of deleting nodes from the tree.
struct DeleteNodesResult {
  // New parent indices after deletion (kNullUint32 for roots).
  std::vector<uint32_t> new_parent_indices;
  // Mapping from old row index to new row index (kNullUint32 if deleted).
  std::vector<uint32_t> old_to_new;
};

// Deletes nodes matching the given spec and reparents their children.
// Children of deleted nodes are reparented to the nearest surviving ancestor.
//
// Algorithm processes nodes in topological order (roots first), so when
// visiting a node, its parent's new index is already computed. This allows
// efficient bulk computation of surviving ancestors without per-node lookups.
//
// Args:
//   data: The tree data containing structure and columns
//   spec: Specification of which nodes to delete
//   pool: StringPool for glob pattern matching on strings
//
// Returns the mapping information for rebuilding the tree.
base::StatusOr<DeleteNodesResult> DeleteNodes(const TreeData& data,
                                              const TreeDeleteSpec& spec,
                                              const StringPool* pool);

// Result of propagate-up operation.
// The tree structure is unchanged; a new column with aggregated values is
// added.
struct PropagateUpResult {
  explicit PropagateUpResult(PassthroughColumn col)
      : out_column(std::move(col)) {}
  PassthroughColumn out_column;
};

// Propagates values from leaves to root using aggregation.
// Each node's output value = agg(node's input value, all children's outputs).
// Processes nodes in reverse topological order (leaves first).
//
// Args:
//   data: The tree data containing structure and columns
//   spec: Specification of which column to aggregate and how
//
// Returns a new column with the aggregated values.
base::StatusOr<PropagateUpResult> PropagateUp(const TreeData& data,
                                              const TreePropagateSpec& spec);

// Result of propagate-down operation.
// The tree structure is unchanged; a new column with propagated values is
// added.
struct PropagateDownResult {
  explicit PropagateDownResult(PassthroughColumn col)
      : out_column(std::move(col)) {}
  PassthroughColumn out_column;
};

// Propagates values from root to leaves using aggregation.
// Each node's output value = agg(parent's output value, node's input value).
// Root nodes use their input value directly (no parent contribution).
// Processes nodes in topological order (roots first).
//
// Args:
//   data: The tree data containing structure and columns
//   spec: Specification of which column to propagate and how
//
// Returns a new column with the propagated values.
base::StatusOr<PropagateDownResult> PropagateDown(
    const TreeData& data,
    const TreePropagateSpec& spec);

// Result of inverting and merging the tree.
// The inversion creates path-based nodes that are then merged by key.
struct InvertAndMergeResult {
  // For each output row, the source row indices that were merged into it.
  // Sources for output row i are at merged_sources[offsets[i]..offsets[i+1]).
  CsrVector<uint32_t> merged_sources;
  // New parent indices after inversion and merging (kNullUint32 for roots).
  std::vector<uint32_t> new_parent_indices;
  // Mapping from old row index to new row index (kNullUint32 if not in output).
  // Note: Unlike delete, a node may map to multiple output nodes (one per
  // path). This maps to the FIRST output node containing this source.
  std::vector<uint32_t> old_to_new;
};

// Inverts the tree and merges by key, producing a bottom-up view.
//
// In the inverted tree:
// - Original leaves become new roots
// - Original parents become children of their former children
// - Nodes appearing in multiple paths are duplicated then merged by key
//
// The algorithm:
// 1. Walk in reverse topological order (leaves first)
// 2. For each node, compute hash = HASH(child_hashes..., key)
// 3. Group by hash to merge nodes with same path signature
// 4. Resolve parent relationships via hash linkage
//
// This correctly handles nodes with multiple children: they appear in
// multiple paths in the inverted tree, merged by (path_prefix, key).
//
// Args:
//   parent_indices: Current parent indices (kNullUint32 for roots)
//   key_values: Key column for merging (nodes with same path+key merge)
//   order_values: Order column for deterministic output
//
// Returns the merged structure with new parent relationships.
template <typename KeyT, typename OrderT>
base::StatusOr<InvertAndMergeResult> InvertAndMerge(
    const std::vector<uint32_t>& parent_indices,
    const std::vector<KeyT>& key_values,
    const std::vector<OrderT>& order_values);

// Explicit instantiation declarations
extern template base::StatusOr<InvertAndMergeResult>
InvertAndMerge<int64_t, int64_t>(const std::vector<uint32_t>& parent_indices,
                                 const std::vector<int64_t>& key_values,
                                 const std::vector<int64_t>& order_values);

extern template base::StatusOr<InvertAndMergeResult>
InvertAndMerge<StringPool::Id, int64_t>(
    const std::vector<uint32_t>& parent_indices,
    const std::vector<StringPool::Id>& key_values,
    const std::vector<int64_t>& order_values);

}  // namespace perfetto::trace_processor::plugins::tree

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_TREE_TREE_ALGORITHMS_H_
