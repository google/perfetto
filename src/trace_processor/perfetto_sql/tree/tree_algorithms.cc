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

#include "src/trace_processor/perfetto_sql/tree/tree_algorithms.h"

#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/span.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/perfetto_sql/tree/column_utils.h"
#include "src/trace_processor/perfetto_sql/tree/tree.h"
#include "src/trace_processor/util/glob.h"

namespace perfetto::trace_processor::plugins::tree {

base::StatusOr<TreeBuildResult> BuildTreeStructure(
    const std::vector<TreeInputRow>& rows) {
  TreeBuildResult result;
  result.node_ids.reserve(rows.size());
  result.parent_indices.reserve(rows.size());

  // Build map from node_id to row index
  base::FlatHashMap<int64_t, uint32_t> id_to_row;
  for (uint32_t i = 0; i < rows.size(); ++i) {
    int64_t id = rows[i].id;
    if (id_to_row.Find(id)) {
      return base::ErrStatus("Duplicate node ID: %" PRId64, id);
    }
    id_to_row[id] = i;
    result.node_ids.push_back(id);
  }

  // Resolve parent IDs to row indices
  for (auto row : rows) {
    if (row.parent_id == kNullInt64) {
      result.parent_indices.push_back(kNullUint32);
    } else {
      int64_t parent_id = row.parent_id;
      auto* parent_row = id_to_row.Find(parent_id);
      if (!parent_row) {
        // Orphan node - parent doesn't exist
        // For now, treat as root (could be an error in strict mode)
        result.parent_indices.push_back(kNullUint32);
      } else {
        result.parent_indices.push_back(*parent_row);
      }
    }
  }
  return result;
}

std::vector<uint32_t> ComputeDepths(
    const std::vector<uint32_t>& parent_indices) {
  const auto n = static_cast<uint32_t>(parent_indices.size());
  std::vector<uint32_t> depths(n, kNullUint32);
  // For each node, walk up to root computing depth
  for (uint32_t i = 0; i < n; ++i) {
    if (depths[i] != kNullUint32) {
      continue;
    }

    // Collect path to root
    std::vector<uint32_t> path;
    uint32_t current = i;
    while (current != kNullUint32 && depths[current] == kNullUint32) {
      path.push_back(current);
      current = parent_indices[current];
    }

    // Compute depths for path (reverse order: from root towards leaf)
    // If we stopped because we found an already-computed ancestor,
    // start one level deeper than that ancestor.
    // If we stopped because we reached a root (no parent), start at depth 0.
    uint32_t depth = 0;
    if (current != kNullUint32) {
      // We stopped because current is already computed (not in path)
      depth = depths[current] + 1;
    }
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
      depths[*it] = depth++;
    }
  }
  return depths;
}

CsrVector<uint32_t> BuildChildrenMap(
    const std::vector<uint32_t>& parent_indices) {
  const auto n = static_cast<uint32_t>(parent_indices.size());

  // First pass: count children per node
  std::vector<uint32_t> child_counts(n, 0);
  for (uint32_t i = 0; i < n; ++i) {
    if (parent_indices[i] != kNullUint32) {
      child_counts[parent_indices[i]]++;
    }
  }

  // Build offsets from counts
  CsrVector<uint32_t> result;
  result.offsets.resize(n + 1);
  result.offsets[0] = 0;
  for (uint32_t i = 0; i < n; ++i) {
    result.offsets[i + 1] = result.offsets[i] + child_counts[i];
  }

  // Allocate data array
  result.data.resize(result.offsets[n]);

  // Second pass: fill children (reuse child_counts as write cursors)
  std::fill(child_counts.begin(), child_counts.end(), 0);
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t parent = parent_indices[i];
    if (parent != kNullUint32) {
      uint32_t pos = result.offsets[parent] + child_counts[parent];
      result.data[pos] = i;
      child_counts[parent]++;
    }
  }

  return result;
}

std::vector<uint32_t> TopologicalOrder(
    const std::vector<uint32_t>& parent_indices) {
  const auto n = static_cast<uint32_t>(parent_indices.size());
  if (n == 0) {
    return {};
  }

  // Build children map
  CsrVector<uint32_t> children_map = BuildChildrenMap(parent_indices);

  // BFS from roots
  std::vector<uint32_t> result;
  result.reserve(n);

  // Find all roots (nodes with no parent)
  for (uint32_t i = 0; i < n; ++i) {
    if (parent_indices[i] == kNullUint32) {
      result.push_back(i);
    }
  }

  // BFS: process queue and add children
  for (size_t head = 0; head < result.size(); ++head) {
    uint32_t node = result[head];
    for (uint32_t child : children_map[node]) {
      result.push_back(child);
    }
  }
  return result;
}

template <>
int64_t ApplyAggregation(const std::vector<int64_t>& values,
                         TreeAggType agg_type) {
  PERFETTO_DCHECK(!values.empty());
  switch (agg_type) {
    case TreeAggType::kMin:
      return *std::min_element(values.begin(), values.end());
    case TreeAggType::kMax:
      return *std::max_element(values.begin(), values.end());
    case TreeAggType::kSum:
      return std::accumulate(values.begin(), values.end(), int64_t{0});
    case TreeAggType::kCount:
      return static_cast<int64_t>(values.size());
    case TreeAggType::kAny:
      return values[0];
  }
  PERFETTO_FATAL("Unknown aggregation type");
}

template <>
double ApplyAggregation(const std::vector<double>& values,
                        TreeAggType agg_type) {
  PERFETTO_DCHECK(!values.empty());
  switch (agg_type) {
    case TreeAggType::kMin:
      return *std::min_element(values.begin(), values.end());
    case TreeAggType::kMax:
      return *std::max_element(values.begin(), values.end());
    case TreeAggType::kSum:
      return std::accumulate(values.begin(), values.end(), 0.0);
    case TreeAggType::kCount:
      return static_cast<double>(values.size());
    case TreeAggType::kAny:
      return values[0];
  }
  PERFETTO_FATAL("Unknown aggregation type");
}

namespace {

// Sorts siblings by order column and merges consecutive siblings with same key.
template <typename KeyT, typename OrderT>
void ProcessSiblingsConsecutive(std::vector<uint32_t>& siblings,
                                uint32_t new_parent_idx,
                                const std::vector<KeyT>& key_values,
                                const std::vector<OrderT>& order_values,
                                MergeSiblingsResult* result) {
  if (siblings.empty()) {
    return;
  }

  std::sort(siblings.begin(), siblings.end(), [&](uint32_t a, uint32_t b) {
    return order_values[a] < order_values[b];
  });

  // Merge only consecutive siblings with same key
  uint32_t group_start = 0;
  while (group_start < siblings.size()) {
    uint32_t group_end = group_start + 1;
    const KeyT& group_key = key_values[siblings[group_start]];

    // Find consecutive run with same key
    while (group_end < siblings.size() &&
           key_values[siblings[group_end]] == group_key) {
      group_end++;
    }

    // Create merged node - new_idx is current number of output nodes
    auto new_idx = static_cast<uint32_t>(result->new_parent_indices.size());
    for (uint32_t i = group_start; i < group_end; ++i) {
      result->merged_sources.Push(siblings[i]);
      result->old_to_new[siblings[i]] = new_idx;
    }
    result->merged_sources.FinishNode();
    result->new_parent_indices.push_back(new_parent_idx);

    group_start = group_end;
  }
}

// Merges all siblings with same key.
template <typename KeyT>
void ProcessSiblingsGlobal(std::vector<uint32_t>& siblings,
                           uint32_t new_parent_idx,
                           const std::vector<KeyT>& key_values,
                           MergeSiblingsResult* result) {
  if (siblings.empty()) {
    return;
  }

  // Merge all siblings with same key
  base::FlatHashMap<KeyT, std::vector<uint32_t>> key_groups;
  for (uint32_t sibling : siblings) {
    const KeyT& key = key_values[sibling];
    auto* group = key_groups.Find(key);
    if (group) {
      group->push_back(sibling);
    } else {
      key_groups[key] = {sibling};
    }
  }

  // Process each group
  for (auto it = key_groups.GetIterator(); it; ++it) {
    auto new_idx = static_cast<uint32_t>(result->new_parent_indices.size());
    for (uint32_t old_idx : it.value()) {
      result->old_to_new[old_idx] = new_idx;
      result->merged_sources.Push(old_idx);
    }
    result->merged_sources.FinishNode();
    result->new_parent_indices.push_back(new_parent_idx);
  }
}

}  // namespace

// Template implementation for MergeSiblings
template <typename KeyT, typename OrderT>
base::StatusOr<MergeSiblingsResult> MergeSiblings(
    const std::vector<uint32_t>& parent_indices,
    const std::vector<KeyT>& key_values,
    const std::vector<OrderT>& order_values,
    TreeMergeMode mode) {
  const uint32_t n = static_cast<uint32_t>(parent_indices.size());
  PERFETTO_DCHECK(key_values.size() == n);
  PERFETTO_DCHECK(order_values.size() == n);

  MergeSiblingsResult result;
  result.old_to_new.resize(n, kNullUint32);
  result.merged_sources.StartBuild();

  // Build children map
  CsrVector<uint32_t> children_map = BuildChildrenMap(parent_indices);

  // Collect root nodes (those with no parent)
  std::vector<uint32_t> roots;
  for (uint32_t i = 0; i < n; ++i) {
    if (parent_indices[i] == kNullUint32) {
      roots.push_back(i);
    }
  }

  // Process roots as siblings
  if (mode == TreeMergeMode::kConsecutive) {
    ProcessSiblingsConsecutive(roots, kNullUint32, key_values, order_values,
                               &result);
  } else {
    ProcessSiblingsGlobal(roots, kNullUint32, key_values, &result);
  }

  // Process children of each node in topological order
  // We iterate through old indices and look up their children
  for (uint32_t parent_idx = 0; parent_idx < n; ++parent_idx) {
    base::Span<const uint32_t> children_span = children_map[parent_idx];
    if (children_span.empty()) {
      continue;
    }
    // Copy children to a mutable vector for sorting
    std::vector<uint32_t> siblings(children_span.begin(), children_span.end());

    // The new parent index is the merged index of the old parent
    uint32_t new_parent_idx = result.old_to_new[parent_idx];
    if (mode == TreeMergeMode::kConsecutive) {
      ProcessSiblingsConsecutive(siblings, new_parent_idx, key_values,
                                 order_values, &result);
    } else {
      ProcessSiblingsGlobal(siblings, new_parent_idx, key_values, &result);
    }
  }
  return result;
}

base::StatusOr<DeleteNodesResult> DeleteNodes(const TreeData& data,
                                              const TreeDeleteSpec& spec,
                                              const StringPool* pool) {
  const uint32_t n = static_cast<uint32_t>(data.parent_indices.size());
  if (n == 0) {
    return DeleteNodesResult{{}, {}};
  }

  // Find the column to match against
  const PassthroughColumn* col =
      FindColumnByName(data.passthrough_columns, spec.column_name);
  if (!col) {
    return base::ErrStatus("Delete column '%s' not found",
                           spec.column_name.c_str());
  }

  // Step 1: Mark nodes to delete (bulk operation over all nodes)
  std::vector<bool> to_delete(n, false);

  if (spec.op == TreeCompareOp::kEq) {
    if (col->IsInt64() && std::holds_alternative<int64_t>(spec.value)) {
      int64_t target = std::get<int64_t>(spec.value);
      const auto& values = col->AsInt64();
      for (uint32_t i = 0; i < n; ++i) {
        to_delete[i] = (values[i] == target);
      }
    } else if (col->IsString() &&
               std::holds_alternative<StringPool::Id>(spec.value)) {
      StringPool::Id target = std::get<StringPool::Id>(spec.value);
      const auto& values = col->AsString();
      for (uint32_t i = 0; i < n; ++i) {
        to_delete[i] = (values[i] == target);
      }
    } else {
      return base::ErrStatus("Delete spec type mismatch with column '%s'",
                             spec.column_name.c_str());
    }
  } else if (spec.op == TreeCompareOp::kGlob) {
    if (!col->IsString()) {
      return base::ErrStatus("Glob comparison requires string column");
    }
    if (!std::holds_alternative<StringPool::Id>(spec.value)) {
      return base::ErrStatus("Glob pattern must be a string");
    }
    StringPool::Id pattern_id = std::get<StringPool::Id>(spec.value);
    base::StringView pattern = pool->Get(pattern_id);
    util::GlobMatcher matcher = util::GlobMatcher::FromPattern(pattern);

    const auto& values = col->AsString();
    for (uint32_t i = 0; i < n; ++i) {
      to_delete[i] = matcher.Matches(pool->Get(values[i]));
    }
  }

  // Step 2: Process in topological order to compute new indices and parents
  // Key insight: when visiting a node, its parent's new index is already known
  auto order = TopologicalOrder(data.parent_indices);

  DeleteNodesResult result;
  result.old_to_new.resize(n, kNullUint32);

  // For each old node, tracks the new index of its nearest surviving ancestor
  // (or kNullUint32 if no surviving ancestor). Updated as we process nodes.
  std::vector<uint32_t> surviving_ancestor(n, kNullUint32);

  uint32_t new_idx = 0;
  for (uint32_t old_idx : order) {
    uint32_t old_parent = data.parent_indices[old_idx];

    // Get the surviving ancestor from parent (already computed due to topo
    // order)
    uint32_t ancestor = (old_parent == kNullUint32)
                            ? kNullUint32
                            : surviving_ancestor[old_parent];

    if (to_delete[old_idx]) {
      // Node is deleted: propagate ancestor to children via surviving_ancestor
      surviving_ancestor[old_idx] = ancestor;
    } else {
      // Node survives: it becomes the new ancestor for its subtree
      result.old_to_new[old_idx] = new_idx;
      result.new_parent_indices.push_back(ancestor);
      surviving_ancestor[old_idx] = new_idx;
      new_idx++;
    }
  }
  return result;
}

base::StatusOr<PropagateUpResult> PropagateUp(const TreeData& data,
                                              const TreePropagateSpec& spec) {
  const uint32_t n = static_cast<uint32_t>(data.parent_indices.size());
  if (n == 0) {
    return PropagateUpResult(
        PassthroughColumn(spec.out_column, std::vector<int64_t>{}));
  }

  // Find input column
  const PassthroughColumn* in_col =
      FindColumnByName(data.passthrough_columns, spec.in_column);
  if (!in_col) {
    return base::ErrStatus("PropagateUp: column '%s' not found",
                           spec.in_column.c_str());
  }

  // Build children map and get reverse topological order (leaves first)
  CsrVector<uint32_t> children = BuildChildrenMap(data.parent_indices);
  auto order = TopologicalOrder(data.parent_indices);
  std::reverse(order.begin(), order.end());

  // Dispatch on column type
  if (in_col->IsInt64()) {
    std::vector<int64_t> out_values(n);
    const auto& in_values = in_col->AsInt64();
    for (uint32_t idx : order) {
      std::vector<int64_t> to_aggregate;
      to_aggregate.push_back(in_values[idx]);
      for (uint32_t child : children[idx]) {
        to_aggregate.push_back(out_values[child]);
      }
      out_values[idx] = ApplyAggregation(to_aggregate, spec.agg_type);
    }
    return PropagateUpResult(
        PassthroughColumn(spec.out_column, std::move(out_values)));
  }
  if (in_col->IsDouble()) {
    std::vector<double> out_values(n);
    const auto& in_values = in_col->AsDouble();
    for (uint32_t idx : order) {
      std::vector<double> to_aggregate;
      to_aggregate.push_back(in_values[idx]);
      for (uint32_t child : children[idx]) {
        to_aggregate.push_back(out_values[child]);
      }
      out_values[idx] = ApplyAggregation(to_aggregate, spec.agg_type);
    }
    return PropagateUpResult(
        PassthroughColumn(spec.out_column, std::move(out_values)));
  }
  return base::ErrStatus(
      "PropagateUp: string columns not supported for aggregation");
}

base::StatusOr<PropagateDownResult> PropagateDown(
    const TreeData& data,
    const TreePropagateSpec& spec) {
  const uint32_t n = static_cast<uint32_t>(data.parent_indices.size());
  if (n == 0) {
    return PropagateDownResult(
        PassthroughColumn(spec.out_column, std::vector<int64_t>{}));
  }

  // Find input column
  const PassthroughColumn* in_col =
      FindColumnByName(data.passthrough_columns, spec.in_column);
  if (!in_col) {
    return base::ErrStatus("PropagateDown: column '%s' not found",
                           spec.in_column.c_str());
  }

  // Get topological order (roots first)
  auto order = TopologicalOrder(data.parent_indices);

  // Dispatch on column type
  if (in_col->IsInt64()) {
    std::vector<int64_t> out_values(n);
    const auto& in_values = in_col->AsInt64();
    for (uint32_t idx : order) {
      uint32_t parent_idx = data.parent_indices[idx];
      if (parent_idx == kNullUint32) {
        out_values[idx] = in_values[idx];
      } else {
        std::vector<int64_t> to_aggregate = {out_values[parent_idx],
                                             in_values[idx]};
        out_values[idx] = ApplyAggregation(to_aggregate, spec.agg_type);
      }
    }
    return PropagateDownResult(
        PassthroughColumn(spec.out_column, std::move(out_values)));
  }
  if (in_col->IsDouble()) {
    std::vector<double> out_values(n);
    const auto& in_values = in_col->AsDouble();
    for (uint32_t idx : order) {
      uint32_t parent_idx = data.parent_indices[idx];
      if (parent_idx == kNullUint32) {
        out_values[idx] = in_values[idx];
      } else {
        std::vector<double> to_aggregate = {out_values[parent_idx],
                                            in_values[idx]};
        out_values[idx] = ApplyAggregation(to_aggregate, spec.agg_type);
      }
    }
    return PropagateDownResult(
        PassthroughColumn(spec.out_column, std::move(out_values)));
  }
  return base::ErrStatus(
      "PropagateDown: string columns not supported for aggregation");
}

template <typename KeyT, typename OrderT>
base::StatusOr<InvertAndMergeResult> InvertAndMerge(
    const std::vector<uint32_t>& parent_indices,
    const std::vector<KeyT>& key_values,
    const std::vector<OrderT>& order_values) {
  const uint32_t n = static_cast<uint32_t>(parent_indices.size());
  PERFETTO_DCHECK(key_values.size() == n);
  PERFETTO_DCHECK(order_values.size() == n);

  InvertAndMergeResult result;
  result.old_to_new.resize(n, kNullUint32);

  if (n == 0) {
    result.merged_sources.StartBuild();
    return result;
  }

  // Build children map to find leaves
  CsrVector<uint32_t> children = BuildChildrenMap(parent_indices);

  // Find all leaves (nodes with no children) - these become roots in inverted
  // tree
  std::vector<uint32_t> leaves;
  for (uint32_t i = 0; i < n; ++i) {
    if (children[i].empty()) {
      leaves.push_back(i);
    }
  }

  // Map: (inverted_parent_idx, key) -> output_idx
  // For roots, inverted_parent_idx = kNullUint32
  base::FlatHashMap<std::pair<uint32_t, KeyT>, uint32_t> merge_map;

  // We build merged_sources incrementally. Since we may need to append to
  // existing nodes (when merging), we use a temporary vector<vector> and
  // convert to CSR at the end.
  std::vector<std::vector<uint32_t>> temp_merged_sources;

  // Walk up from each leaf, creating/merging nodes as we go
  for (uint32_t leaf : leaves) {
    // The inverted parent starts as null (leaf becomes root)
    uint32_t inverted_parent = kNullUint32;

    // Walk from leaf up to original root
    uint32_t current = leaf;
    while (current != kNullUint32) {
      KeyT key = key_values[current];
      auto map_key = std::make_pair(inverted_parent, key);

      auto* existing = merge_map.Find(map_key);
      uint32_t output_idx;

      if (existing) {
        // Merge with existing node
        output_idx = *existing;
        temp_merged_sources[output_idx].push_back(current);
      } else {
        // Create new output node
        output_idx = static_cast<uint32_t>(temp_merged_sources.size());
        temp_merged_sources.push_back({current});
        result.new_parent_indices.push_back(inverted_parent);
        merge_map[map_key] = output_idx;
      }

      // Track first mapping for this source node
      if (result.old_to_new[current] == kNullUint32) {
        result.old_to_new[current] = output_idx;
      }

      // Move up: this node becomes the inverted parent for the next iteration
      inverted_parent = output_idx;
      current = parent_indices[current];
    }
  }

  // Convert temp_merged_sources to CSR format
  result.merged_sources.StartBuild();
  for (const auto& sources : temp_merged_sources) {
    for (uint32_t src : sources) {
      result.merged_sources.Push(src);
    }
    result.merged_sources.FinishNode();
  }

  return result;
}

// Explicit template instantiations
template base::StatusOr<MergeSiblingsResult> MergeSiblings<int64_t, int64_t>(
    const std::vector<uint32_t>& parent_indices,
    const std::vector<int64_t>& key_values,
    const std::vector<int64_t>& order_values,
    TreeMergeMode mode);
template base::StatusOr<MergeSiblingsResult>
MergeSiblings<StringPool::Id, int64_t>(
    const std::vector<uint32_t>& parent_indices,
    const std::vector<StringPool::Id>& key_values,
    const std::vector<int64_t>& order_values,
    TreeMergeMode mode);
template base::StatusOr<InvertAndMergeResult> InvertAndMerge<int64_t, int64_t>(
    const std::vector<uint32_t>& parent_indices,
    const std::vector<int64_t>& key_values,
    const std::vector<int64_t>& order_values);
template base::StatusOr<InvertAndMergeResult>
InvertAndMerge<StringPool::Id, int64_t>(
    const std::vector<uint32_t>& parent_indices,
    const std::vector<StringPool::Id>& key_values,
    const std::vector<int64_t>& order_values);

}  // namespace perfetto::trace_processor::plugins::tree
