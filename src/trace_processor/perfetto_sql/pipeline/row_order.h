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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_ROW_ORDER_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_ROW_ORDER_H_

#include <cstdint>
#include <optional>
#include <vector>

#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"

namespace perfetto::trace_processor::pipeline {

// Adds the steps RowOrder decides rows need. Lowering adds executor steps for
// them.
class OrderSteps {
 public:
  virtual ~OrderSteps();

  // Makes the columns of `keys` comparable, before a step sorts by them.
  virtual void MakeComparable(const std::vector<SortKey>& keys) = 0;

  // Sorts the rows by `keys`.
  virtual void Sort(const std::vector<SortKey>& keys) = 0;

  // Removes the Sort added last. No step added since reads its order.
  virtual void RemoveSort() = 0;

  // Groups the rows by `keys`, sorts each group by `sorted_by` and numbers
  // the groups.
  virtual void GroupedSort(const std::vector<ColumnId>& keys,
                           const std::vector<SortKey>& sorted_by) = 0;

  // Numbers the nodes of the tree of `node` and `parent`.
  virtual void NumberTree(ColumnId node, ColumnId parent) = 0;

  // Orders the rows as a walk in `direction` of the numbered tree.
  virtual void WalkTree(TreeDirection direction) = 0;

  // Combines small batches into full ones.
  virtual void CombineBatches() = 0;
};

// What is known about the order of a pipeline's rows, and the decisions of
// which sorts, groupings and tree walks make them meet a requirement. Knows
// nothing of where columns are in a batch.
//
// Require... methods add steps to `steps` only if needed. Declare... methods
// record what a step added by an operation produces.
class RowOrder {
 public:
  // A known layout of the rows.
  struct Arrangement {
    // The columns the rows are grouped by. Empty means one group.
    std::vector<ColumnId> grouped_by;

    // The keys each group is sorted by.
    std::vector<SortKey> sorted_by;
  };

  // The tree the rows are numbered as.
  struct Tree {
    ColumnId node;
    ColumnId parent;
  };

  // ---------------------------------------------------------------------------
  // Requirements: add steps only if needed
  // ---------------------------------------------------------------------------

  // Sorts rows by `keys`.
  void RequireSorted(const std::vector<SortKey>& keys, OrderSteps* steps);

  // Groups rows by `keys`, so rows with equal keys are next to each other.
  void RequireGrouped(const std::vector<ColumnId>& keys, OrderSteps* steps);

  // Groups rows by `keys` and sorts each group by `sorted_by`.
  void RequireSortedWithinGroups(const std::vector<ColumnId>& keys,
                                 const std::vector<SortKey>& sorted_by,
                                 OrderSteps* steps);

  // Groups rows by `keys` and returns the index, in the order they were
  // made, of the group numbers for them.
  uint32_t RequireGroupNumbers(const std::vector<ColumnId>& keys,
                               OrderSteps* steps);

  // Orders rows as a walk in `direction` of the tree of `node` and `parent`.
  void RequireTreeWalk(ColumnId node,
                       ColumnId parent,
                       TreeDirection direction,
                       OrderSteps* steps);

  // Makes batches full, if an earlier step may have made small ones.
  void RequireFullBatches(OrderSteps* steps);

  // ---------------------------------------------------------------------------
  // Declarations: record what a step produces
  // ---------------------------------------------------------------------------

  // Records that a step read the order of the rows, so the last Sort can no
  // longer be removed.
  void DeclareOrderRead() { open_sort_.reset(); }

  void DeclareSorted(const std::vector<SortKey>& keys);
  void DeclareGrouped(const std::vector<ColumnId>& keys);
  void DeclareSortedWithinGroups(const std::vector<ColumnId>& keys,
                                 const std::vector<SortKey>& sorted_by);
  void DeclareSmallBatches() { full_batches_ = false; }

  // Records that rows were dropped. A tree numbered before may now name
  // parents which are gone, so the next tree walk numbers it again.
  void DeclareRowsDropped();

  // Records group numbers for `keys`, made after any recorded so far.
  void DeclareGroupNumbers(const std::vector<ColumnId>& keys);

  // Records that the rows are a numbered walk in `direction` of the tree of
  // `node` and `parent`.
  void DeclareTreeWalk(ColumnId node, ColumnId parent, TreeDirection);

  // Forgets everything, for a step which outputs new rows.
  void Reset() { *this = RowOrder(); }

  // ---------------------------------------------------------------------------
  // Queries
  // ---------------------------------------------------------------------------

  // Whether rows are known to be grouped by `keys` and sorted by `sorted_by`
  // within each group. Sorting by some keys first also groups by them. For
  // example, rows sorted by (cpu, ts) are grouped by cpu and sorted by ts in
  // each group.
  bool IsArranged(const std::vector<ColumnId>& keys,
                  const std::vector<SortKey>& sorted_by) const;

  // The longest prefix of the known sort order that uses only `columns`.
  std::vector<SortKey> SortedBy(const std::vector<ColumnId>& columns) const;

  // The tree the rows are numbered as, if any.
  const std::optional<Tree>& tree() const { return tree_; }

  // The direction of the walk of tree() the rows are in, if any.
  const std::optional<TreeDirection>& tree_walk() const { return tree_walk_; }

 private:
  // What was known before the last Sort, while no step reads its order.
  struct OpenSort {
    std::vector<Arrangement> arrangements;
    std::optional<TreeDirection> tree_walk;
  };

  // Removes the open Sort, if there is one. Returns whether one was removed.
  bool DropOpenSort(OrderSteps* steps);

  // Adds a Sort by `keys`, which can be removed until a step reads its order.
  void AddSort(const std::vector<SortKey>& keys, OrderSteps* steps);

  // Adds a GroupedSort by `keys`, each group sorted by `sorted_by`.
  void GroupRows(const std::vector<ColumnId>& keys,
                 const std::vector<SortKey>& sorted_by,
                 OrderSteps* steps);

  // Every layout known to hold for the rows.
  std::vector<Arrangement> arrangements_;

  // The direction of the walk of `tree_` the rows are in, if any.
  std::optional<TreeDirection> tree_walk_;

  // The tree the rows are numbered as, if any. Numbers stay valid when rows
  // are reordered, until rows are dropped or another tree is numbered.
  std::optional<Tree> tree_;

  // The keys of each set of group numbers, in the order they were made.
  // Numbers stay valid when rows are reordered.
  std::vector<std::vector<ColumnId>> group_numbers_;

  // Whether batches are known to be full.
  bool full_batches_ = true;

  // The last Sort, if it can still be removed.
  std::optional<OpenSort> open_sort_;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_ROW_ORDER_H_
