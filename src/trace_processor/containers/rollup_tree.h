/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_CONTAINERS_ROLLUP_TREE_H_
#define SRC_TRACE_PROCESSOR_CONTAINERS_ROLLUP_TREE_H_

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace perfetto::trace_processor {

// A value that can be stored in a rollup node (mirrors SQL types).
using RollupValue = std::variant<std::monostate,  // NULL
                                 int64_t,         // INTEGER
                                 double,          // REAL
                                 std::string>;    // TEXT

// Sort specification for ordering nodes.
struct RollupSortSpec {
  // Which aggregate to sort by (only used when hierarchy_level < 0).
  int agg_index = 0;

  // Which hierarchy level to sort by group value.
  // When >= 0, all levels sort by hierarchy value (alphabetically).
  // The specified level uses 'descending', others use ASC.
  // When < 0, all levels sort by agg_index.
  // Default: 0 (sort all levels alphabetically, level 0 uses direction).
  int hierarchy_level = 0;

  // Sort direction (default ASC for alphabetical sorting)
  bool descending = false;
};

// Options for flattening the tree into a list of visible rows.
struct RollupFlattenOptions {
  // IDs of nodes to expand (allowlist mode) or collapse (denylist mode).
  std::set<int64_t> ids;

  // If true, 'ids' contains nodes to collapse (all others expanded).
  // If false, 'ids' contains nodes to expand (all others collapsed).
  bool denylist_mode = false;

  // Sort specification
  RollupSortSpec sort;

  // Pagination
  int offset = 0;
  int limit = std::numeric_limits<int>::max();

  // Depth filtering (applied efficiently during traversal)
  // min_depth: exclude nodes with depth < min_depth (e.g., 1 to exclude root)
  // max_depth: exclude nodes with depth > max_depth and don't recurse deeper
  int min_depth = 0;
  int max_depth = std::numeric_limits<int>::max();
};

// A flattened row from the rollup tree, ready for output.
struct RollupFlatRow {
  int64_t id = 0;
  int64_t parent_id = -1;  // -1 means no parent (root's children)
  int depth = 0;
  int child_count = 0;

  // Hierarchy values at each level
  std::vector<RollupValue> hierarchy_values;

  // Aggregate values
  std::vector<RollupValue> aggregates;
};

// Internal tree node structure.
struct RollupNode {
  int64_t id = 0;
  int level = -1;  // -1 for root, 0+ for hierarchy levels

  // Hierarchy column values at each level
  std::vector<RollupValue> hierarchy_values;

  // Aggregate values
  std::vector<RollupValue> aggs;

  // Tree structure
  RollupNode* parent = nullptr;
  std::vector<std::unique_ptr<RollupNode>> children;

  // Query-time state (not persisted across queries)
  bool expanded = false;
};

// A hierarchical rollup tree that supports expand/collapse navigation.
//
// This class maintains a tree of aggregated data where each level groups
// by a different hierarchy column (like SQL ROLLUP). It provides methods to:
// - Build the tree by adding rows at different hierarchy levels
// - Sort children at each level
// - Flatten the tree into a list of visible rows based on expansion state
//
// The class is agnostic to how it's populated - it can be built from SQL
// query results, in-memory data, or any other source.
//
// Example usage:
//   RollupTree tree({"category", "item"}, 2);  // 2 aggregates
//
//   // Add hierarchy level 0 (category totals)
//   tree.AddRow(0, {"fruit"}, {RollupValue(45), RollupValue(3)});
//   tree.AddRow(0, {"vegetable"}, {RollupValue(25), RollupValue(3)});
//
//   // Add hierarchy level 1 (item details)
//   tree.AddRow(1, {"fruit", "apple"}, {RollupValue(30), RollupValue(2)});
//   tree.AddRow(1, {"fruit", "banana"}, {RollupValue(15), RollupValue(1)});
//
//   // Set root aggregates (grand total)
//   tree.SetRootAggregates({RollupValue(70), RollupValue(6)});
//
//   // Get flattened rows with all nodes expanded
//   RollupFlattenOptions opts;
//   opts.denylist_mode = true;  // expand all
//   auto rows = tree.GetRows(opts);
//
class RollupTree {
 public:
  // Creates a rollup tree with the given hierarchy column names and
  // number of aggregate columns.
  RollupTree(std::vector<std::string> hierarchy_cols, size_t num_aggregates);

  ~RollupTree();

  // Non-copyable, movable
  RollupTree(const RollupTree&) = delete;
  RollupTree& operator=(const RollupTree&) = delete;
  RollupTree(RollupTree&&) noexcept;
  RollupTree& operator=(RollupTree&&) noexcept;

  // Building the tree

  // Adds a row at the specified hierarchy level.
  // - level: 0 for first grouping level, 1 for second, etc.
  // - hierarchy_path: values for hierarchy columns up to and including 'level'
  // - aggregates: aggregate values for this group
  void AddRow(int level,
              const std::vector<RollupValue>& hierarchy_path,
              std::vector<RollupValue> aggregates);

  // Sets the root node's aggregates (grand total across all data).
  void SetRootAggregates(std::vector<RollupValue> aggregates);

  // Querying

  // Returns flattened rows based on the given options.
  // The tree is sorted and flattened according to expansion state.
  std::vector<RollupFlatRow> GetRows(const RollupFlattenOptions& options);

  // Returns the total number of visible rows (before pagination).
  int GetTotalRows(const RollupFlattenOptions& options);

  // Accessors

  const std::vector<std::string>& hierarchy_cols() const {
    return hierarchy_cols_;
  }

  size_t num_aggregates() const { return num_aggregates_; }

  int total_nodes() const { return total_nodes_; }

 private:
  // Finds or creates a node at the given path in the tree.
  RollupNode* FindOrCreateNode(const std::vector<RollupValue>& segments,
                               int level);

  // Sorts all children recursively using the given spec.
  void SortTree(RollupNode* node, const RollupSortSpec& spec);

  // Flattens the tree into the output vector based on expansion state.
  void FlattenTree(RollupNode* node,
                   const std::set<int64_t>& ids,
                   bool denylist_mode,
                   int min_depth,
                   int max_depth,
                   std::vector<RollupNode*>* out);

  // Converts a RollupNode to a RollupFlatRow.
  RollupFlatRow NodeToFlatRow(const RollupNode* node) const;

  std::vector<std::string> hierarchy_cols_;
  size_t num_aggregates_;
  std::unique_ptr<RollupNode> root_;
  int64_t next_id_ = 1;  // 0 is reserved for root
  int total_nodes_ = 0;

  // Cached sort state to avoid redundant re-sorting
  std::string cached_sort_spec_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_CONTAINERS_ROLLUP_TREE_H_
