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

#ifndef SRC_TRACE_PROCESSOR_CONTAINERS_PIVOT_TABLE_H_
#define SRC_TRACE_PROCESSOR_CONTAINERS_PIVOT_TABLE_H_

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace perfetto::trace_processor {

// A value that can be stored in a pivot node (mirrors SQL types).
using PivotValue = std::variant<std::monostate,  // NULL
                                int64_t,         // INTEGER
                                double,          // REAL
                                std::string>;    // TEXT

// Sort specification for ordering nodes.
struct PivotSortSpec {
  // Which aggregate to sort by (-1 for sorting by name/hierarchy value)
  int agg_index = 0;

  // Sort direction
  bool descending = true;
};

// Options for flattening the tree into a list of visible rows.
struct PivotFlattenOptions {
  // IDs of nodes to expand (allowlist mode) or collapse (denylist mode).
  std::set<int64_t> ids;

  // If true, 'ids' contains nodes to collapse (all others expanded).
  // If false, 'ids' contains nodes to expand (all others collapsed).
  bool denylist_mode = false;

  // Sort specification
  PivotSortSpec sort;

  // Pagination
  int offset = 0;
  int limit = std::numeric_limits<int>::max();
};

// A flattened row from the pivot table, ready for output.
struct PivotFlatRow {
  int64_t id = 0;
  int64_t parent_id = -1;  // -1 means no parent (root's children)
  int depth = 0;
  bool has_children = false;
  int child_count = 0;

  // Hierarchy values at each level (empty string = NULL)
  std::vector<std::string> hierarchy_values;

  // Aggregate values
  std::vector<PivotValue> aggregates;
};

// Internal tree node structure.
struct PivotNode {
  int64_t id = 0;
  int level = -1;  // -1 for root, 0+ for hierarchy levels

  // Hierarchy column values at each level
  std::vector<std::string> hierarchy_values;

  // Aggregate values
  std::vector<PivotValue> aggs;

  // Tree structure
  PivotNode* parent = nullptr;
  std::vector<std::unique_ptr<PivotNode>> children;

  // Query-time state (not persisted across queries)
  bool expanded = false;
};

// A hierarchical pivot table that supports expand/collapse navigation.
//
// This class maintains a tree of aggregated data where each level groups
// by a different hierarchy column. It provides methods to:
// - Build the tree by adding rows at different hierarchy levels
// - Sort children at each level
// - Flatten the tree into a list of visible rows based on expansion state
//
// The class is agnostic to how it's populated - it can be built from SQL
// query results, in-memory data, or any other source.
//
// Example usage:
//   PivotTable table({"category", "item"}, 2);  // 2 aggregates
//
//   // Add hierarchy level 0 (category totals)
//   table.AddRow(0, {"fruit"}, {PivotValue(45), PivotValue(3)});
//   table.AddRow(0, {"vegetable"}, {PivotValue(25), PivotValue(3)});
//
//   // Add hierarchy level 1 (item details)
//   table.AddRow(1, {"fruit", "apple"}, {PivotValue(30), PivotValue(2)});
//   table.AddRow(1, {"fruit", "banana"}, {PivotValue(15), PivotValue(1)});
//
//   // Set root aggregates (grand total)
//   table.SetRootAggregates({PivotValue(70), PivotValue(6)});
//
//   // Get flattened rows with all nodes expanded
//   PivotFlattenOptions opts;
//   opts.denylist_mode = true;  // expand all
//   auto rows = table.GetRows(opts);
//
class PivotTable {
 public:
  // Creates a pivot table with the given hierarchy column names and
  // number of aggregate columns.
  PivotTable(std::vector<std::string> hierarchy_cols, size_t num_aggregates);

  ~PivotTable();

  // Non-copyable, movable
  PivotTable(const PivotTable&) = delete;
  PivotTable& operator=(const PivotTable&) = delete;
  PivotTable(PivotTable&&) noexcept;
  PivotTable& operator=(PivotTable&&) noexcept;

  // Building the tree

  // Adds a row at the specified hierarchy level.
  // - level: 0 for first grouping level, 1 for second, etc.
  // - hierarchy_path: values for hierarchy columns up to and including 'level'
  // - aggregates: aggregate values for this group
  void AddRow(int level,
              const std::vector<std::string>& hierarchy_path,
              std::vector<PivotValue> aggregates);

  // Sets the root node's aggregates (grand total across all data).
  void SetRootAggregates(std::vector<PivotValue> aggregates);

  // Querying

  // Returns flattened rows based on the given options.
  // The tree is sorted and flattened according to expansion state.
  std::vector<PivotFlatRow> GetRows(const PivotFlattenOptions& options);

  // Returns the total number of visible rows (before pagination).
  int GetTotalRows(const PivotFlattenOptions& options);

  // Accessors

  const std::vector<std::string>& hierarchy_cols() const {
    return hierarchy_cols_;
  }

  size_t num_aggregates() const { return num_aggregates_; }

  int total_nodes() const { return total_nodes_; }

 private:
  // Finds or creates a node at the given path in the tree.
  PivotNode* FindOrCreateNode(const std::vector<std::string>& segments,
                              int level);

  // Sorts all children recursively using the given spec.
  void SortTree(PivotNode* node, const PivotSortSpec& spec);

  // Flattens the tree into the output vector based on expansion state.
  void FlattenTree(PivotNode* node,
                   const std::set<int64_t>& ids,
                   bool denylist_mode,
                   std::vector<PivotNode*>* out);

  // Converts a PivotNode to a PivotFlatRow.
  PivotFlatRow NodeToFlatRow(const PivotNode* node) const;

  std::vector<std::string> hierarchy_cols_;
  size_t num_aggregates_;
  std::unique_ptr<PivotNode> root_;
  int64_t next_id_ = 1;  // 0 is reserved for root
  int total_nodes_ = 0;

  // Cached sort state to avoid redundant re-sorting
  std::string cached_sort_spec_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_CONTAINERS_PIVOT_TABLE_H_
