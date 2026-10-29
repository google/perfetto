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

#include "src/trace_processor/perfetto_sql/pipeline/row_order.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"

namespace perfetto::trace_processor::pipeline {
namespace {

bool Contains(const std::vector<ColumnId>& columns, ColumnId column) {
  return std::find(columns.begin(), columns.end(), column) != columns.end();
}

// Returns `keys` without the keys on columns in `drop`.
std::vector<SortKey> Without(const std::vector<SortKey>& keys,
                             const std::vector<ColumnId>& drop) {
  std::vector<SortKey> kept;
  for (const SortKey& key : keys) {
    if (!Contains(drop, key.column)) {
      kept.push_back(key);
    }
  }
  return kept;
}

}  // namespace

OrderSteps::~OrderSteps() = default;

void RowOrder::RequireSorted(const std::vector<SortKey>& keys,
                             OrderSteps* steps) {
  if (IsArranged({}, keys)) {
    return;
  }
  if (DropOpenSort(steps) && IsArranged({}, keys)) {
    return;
  }
  AddSort(keys, steps);
}

void RowOrder::RequireGrouped(const std::vector<ColumnId>& keys,
                              OrderSteps* steps) {
  if (IsArranged(keys, {})) {
    return;
  }
  if (DropOpenSort(steps) && IsArranged(keys, {})) {
    return;
  }
  GroupRows(keys, {}, steps);
}

void RowOrder::RequireSortedWithinGroups(const std::vector<ColumnId>& keys,
                                         const std::vector<SortKey>& sorted_by,
                                         OrderSteps* steps) {
  // Without keys, all rows are one group, so this is a plain sort.
  if (keys.empty()) {
    RequireSorted(sorted_by, steps);
    return;
  }
  if (IsArranged(keys, sorted_by)) {
    return;
  }
  if (DropOpenSort(steps) && IsArranged(keys, sorted_by)) {
    return;
  }
  // Grouping is stable, so if the rows are already sorted by `sorted_by`,
  // grouping them is enough.
  GroupRows(keys,
            IsArranged({}, sorted_by) ? std::vector<SortKey>() : sorted_by,
            steps);
}

uint32_t RowOrder::RequireGroupNumbers(const std::vector<ColumnId>& keys,
                                       OrderSteps* steps) {
  RequireGrouped(keys, steps);
  for (uint32_t i = 0; i < group_numbers_.size(); ++i) {
    const std::vector<ColumnId>& numbered = group_numbers_[i];
    if (std::is_permutation(numbered.begin(), numbered.end(), keys.begin(),
                            keys.end())) {
      return i;
    }
  }
  // The rows are grouped but have no group numbers. Grouping them again
  // adds the numbers.
  GroupRows(keys, {}, steps);
  return static_cast<uint32_t>(group_numbers_.size() - 1);
}

void RowOrder::RequireTreeWalk(ColumnId node,
                               ColumnId parent,
                               TreeDirection direction,
                               OrderSteps* steps) {
  bool same_tree = tree_ && tree_->node == node && tree_->parent == parent;
  if (same_tree && tree_walk_ == direction) {
    return;
  }
  if (same_tree && DropOpenSort(steps) && tree_walk_ == direction) {
    return;
  }
  if (!same_tree) {
    steps->NumberTree(node, parent);
    tree_ = Tree{node, parent};
  }
  if (direction == TreeDirection::kUp) {
    RequireFullBatches(steps);
    steps->WalkTree(direction);
  } else {
    steps->WalkTree(direction);
    // Rows are output as their parents arrive, so batches can be small.
    full_batches_ = false;
  }
  open_sort_.reset();
  arrangements_.clear();
  tree_walk_ = direction;
}

void RowOrder::RequireFullBatches(OrderSteps* steps) {
  if (!full_batches_) {
    steps->CombineBatches();
    full_batches_ = true;
  }
}

void RowOrder::DeclareSorted(const std::vector<SortKey>& keys) {
  arrangements_.push_back({{}, keys});
}

void RowOrder::DeclareGrouped(const std::vector<ColumnId>& keys) {
  arrangements_.push_back({keys, {}});
}

void RowOrder::DeclareSortedWithinGroups(
    const std::vector<ColumnId>& keys,
    const std::vector<SortKey>& sorted_by) {
  arrangements_.push_back({keys, sorted_by});
}

void RowOrder::DeclareRowsDropped() {
  tree_walk_.reset();
  tree_.reset();
}

void RowOrder::DeclareGroupNumbers(const std::vector<ColumnId>& keys) {
  group_numbers_.push_back(keys);
}

void RowOrder::DeclareTreeWalk(ColumnId node,
                               ColumnId parent,
                               TreeDirection direction) {
  tree_ = Tree{node, parent};
  tree_walk_ = direction;
}

bool RowOrder::IsArranged(const std::vector<ColumnId>& keys,
                          const std::vector<SortKey>& sorted_by) const {
  // Sort keys on group columns are constant within a group, so ignore them.
  std::vector<SortKey> wanted = Without(sorted_by, keys);
  if (keys.empty() && wanted.empty()) {
    return true;
  }
  for (const Arrangement& arrangement : arrangements_) {
    // An arrangement grouped by a subset of `keys` can still match if it is
    // sorted by the remaining keys first.
    if (!std::all_of(arrangement.grouped_by.begin(),
                     arrangement.grouped_by.end(),
                     [&](ColumnId column) { return Contains(keys, column); })) {
      continue;
    }
    std::vector<SortKey> sorted =
        Without(arrangement.sorted_by, arrangement.grouped_by);
    std::vector<ColumnId> missing = arrangement.grouped_by;
    size_t next = 0;
    while (next < sorted.size() && Contains(keys, sorted[next].column)) {
      missing.push_back(sorted[next++].column);
    }
    if (!std::is_permutation(missing.begin(), missing.end(), keys.begin(),
                             keys.end())) {
      continue;
    }
    std::vector<SortKey> rest(sorted.begin() + static_cast<ptrdiff_t>(next),
                              sorted.end());
    if (rest.size() >= wanted.size() &&
        std::equal(wanted.begin(), wanted.end(), rest.begin(),
                   [](const SortKey& a, const SortKey& b) {
                     return a.column == b.column &&
                            a.descending == b.descending;
                   })) {
      return true;
    }
  }
  return false;
}

std::vector<SortKey> RowOrder::SortedBy(
    const std::vector<ColumnId>& columns) const {
  std::vector<SortKey> longest;
  for (const Arrangement& arrangement : arrangements_) {
    if (!arrangement.grouped_by.empty()) {
      continue;
    }
    std::vector<SortKey> keys;
    for (const SortKey& key : arrangement.sorted_by) {
      if (!Contains(columns, key.column)) {
        break;
      }
      keys.push_back(key);
    }
    if (keys.size() > longest.size()) {
      longest = std::move(keys);
    }
  }
  return longest;
}

bool RowOrder::DropOpenSort(OrderSteps* steps) {
  if (!open_sort_) {
    return false;
  }
  steps->RemoveSort();
  arrangements_ = std::move(open_sort_->arrangements);
  tree_walk_ = open_sort_->tree_walk;
  open_sort_.reset();
  return true;
}

void RowOrder::AddSort(const std::vector<SortKey>& keys, OrderSteps* steps) {
  steps->MakeComparable(keys);
  RequireFullBatches(steps);
  steps->Sort(keys);
  open_sort_ = OpenSort{std::move(arrangements_), tree_walk_};
  arrangements_ = {{{}, keys}};
  tree_walk_.reset();
}

void RowOrder::GroupRows(const std::vector<ColumnId>& keys,
                         const std::vector<SortKey>& sorted_by,
                         OrderSteps* steps) {
  steps->MakeComparable(sorted_by);
  RequireFullBatches(steps);
  steps->GroupedSort(keys, sorted_by);
  open_sort_.reset();
  std::vector<Arrangement> kept;
  if (sorted_by.empty()) {
    // Grouping is stable, so an order known within groups of a subset of
    // `keys` still holds within the new groups.
    for (const Arrangement& arrangement : arrangements_) {
      if (std::all_of(
              arrangement.grouped_by.begin(), arrangement.grouped_by.end(),
              [&](ColumnId column) { return Contains(keys, column); })) {
        kept.push_back({keys, arrangement.sorted_by});
      }
    }
  }
  kept.push_back({keys, sorted_by});
  arrangements_ = std::move(kept);
  tree_walk_.reset();
  group_numbers_.push_back(keys);
}

}  // namespace perfetto::trace_processor::pipeline
