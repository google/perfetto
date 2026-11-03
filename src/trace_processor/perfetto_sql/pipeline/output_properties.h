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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OUTPUT_PROPERTIES_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OUTPUT_PROPERTIES_H_

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"

namespace perfetto::trace_processor::pipeline {

// What is known about the batches a node outputs: where each column is, how the
// rows are arranged, the tree they are numbered as and the group number columns
// they carry. Column types are kept apart, by PlanLowering.
struct OutputProperties {
  // A layout the rows are known to have.
  struct Arrangement {
    // The columns the rows are grouped by. Empty means one group.
    std::vector<ColumnId> grouped_by;

    // The keys each group is sorted by.
    std::vector<SortKey> sorted_by;
  };

  // The tree the rows are numbered as.
  struct Tree {
    ColumnId node = 0;
    ColumnId parent = 0;
    // The direction the rows walk the tree in, if they do.
    std::optional<TreeDirection> walk;
    // The positions of the node number columns.
    uint32_t node_position = 0;
    uint32_t parent_position = 0;
  };

  // A column holding group numbers.
  struct GroupNumbers {
    // The columns the groups are formed by.
    std::vector<ColumnId> keys;

    // The position of the group number column.
    uint32_t position = 0;
  };

  // What was known before the most recent sort.
  struct BeforeOpenSort {
    std::vector<Arrangement> arrangements;
    std::optional<TreeDirection> walk;
  };

  // The steps a pipeline must take for a requirement to hold, in this order.
  struct RequiredSteps {
    bool drop_open_sort = false;
    bool number_tree = false;
    bool combine_batches = false;
    bool reorder = false;
  };

  // Whether the rows are grouped by `keys` and sorted by `sorted_by` within
  // each group. Sorting by some keys first also groups by them. For example,
  // rows sorted by (cpu, ts) are grouped by cpu and sorted by ts in each group.
  bool IsArranged(const std::vector<ColumnId>& keys,
                  const std::vector<SortKey>& sorted_by) const;

  // The longest prefix of a known sort order that uses only `columns`.
  std::vector<SortKey> SortedBy(const std::vector<ColumnId>& columns) const;

  // The direction the rows walk the tree of `node` and `parent` in, if known.
  std::optional<TreeDirection> TreeWalk(ColumnId node, ColumnId parent) const;

  // Nothing known yet, of batches from a plan with `plan_columns` columns.
  static OutputProperties Empty(size_t plan_columns);

  // Makes the rows sorted by `keys`, and returns what that takes.
  RequiredSteps RequireSorted(const std::vector<SortKey>& keys);

  // Makes the rows walk the tree of `node` and `parent` in `direction`, and
  // returns what that takes.
  RequiredSteps RequireTreeWalk(ColumnId node,
                                ColumnId parent,
                                TreeDirection direction);

  // The rows are sorted by `keys`, which nothing has read yet.
  void Sort(const std::vector<SortKey>& keys);

  // The sort nothing has read is undone. Returns whether there was one.
  bool DropOpenSort();

  // Something now depends on the order of the rows, so no sort can be undone.
  void CloseOpenSort();

  // The rows are grouped by `keys`, each group sorted by `sorted_by`, and carry
  // a new group number column. Grouping is stable.
  void Group(const std::vector<ColumnId>& keys,
             const std::vector<SortKey>& sorted_by);

  // The rows are numbered as the tree of `node` and `parent`, in two new
  // columns, unless they already are.
  void NumberTree(ColumnId node, ColumnId parent);

  // The rows walk the tree of `node` and `parent` in `direction`, numbering it
  // if needed.
  void WalkTree(ColumnId node, ColumnId parent, TreeDirection direction);

  // Some rows were dropped by a step reading their order. Rows kept stay in
  // order, in batches which can be small, but a tree numbered before may now
  // name parents which are gone.
  void DropRows();

  // The next batch column is `id`. Returns its position.
  uint32_t AddColumn(ColumnId id);

  // Reserves the next batch column for a value with no logical column.
  uint32_t AddTemporaryColumn();

  // The batches hold new rows: nothing is known of them yet.
  void Reset();

  // The number of columns in a batch, including temporary columns.
  uint32_t column_count = 0;

  // The batch position of each logical column, or kNoPosition.
  static constexpr uint32_t kNoPosition = std::numeric_limits<uint32_t>::max();
  std::vector<uint32_t> positions;

  // Whether batches are known to be full, so no combining is needed.
  bool full_batches = true;

  // Every layout known to hold.
  std::vector<Arrangement> arrangements;

  // The tree the rows are numbered as, if any.
  std::optional<Tree> tree;

  // The group number columns. They stay valid when rows are reordered, until
  // Reset.
  std::vector<GroupNumbers> group_numbers;

  // Set while a sort can still be undone.
  std::optional<BeforeOpenSort> before_open_sort;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OUTPUT_PROPERTIES_H_
