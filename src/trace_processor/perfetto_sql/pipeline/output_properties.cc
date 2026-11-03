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

#include "src/trace_processor/perfetto_sql/pipeline/output_properties.h"

#include <algorithm>
#include <cstddef>
#include <optional>
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

bool OutputProperties::IsArranged(const std::vector<ColumnId>& keys,
                                  const std::vector<SortKey>& sorted_by) const {
  // Sort keys on group columns are constant within a group, so ignore them.
  std::vector<SortKey> wanted = Without(sorted_by, keys);
  if (keys.empty() && wanted.empty()) {
    return true;
  }
  for (const Arrangement& arrangement : arrangements) {
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

std::vector<SortKey> OutputProperties::SortedBy(
    const std::vector<ColumnId>& columns) const {
  std::vector<SortKey> longest;
  for (const Arrangement& arrangement : arrangements) {
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

std::optional<TreeDirection> OutputProperties::TreeWalk(ColumnId node,
                                                        ColumnId parent) const {
  if (!tree || tree->node != node || tree->parent != parent) {
    return std::nullopt;
  }
  return tree->walk;
}

void OutputProperties::Sort(const std::vector<SortKey>& keys) {
  std::optional<TreeDirection> walk;
  if (tree) {
    walk = tree->walk;
    tree->walk.reset();
  }
  before_open_sort = BeforeOpenSort{std::move(arrangements), walk};
  arrangements = {{{}, keys}};
}

bool OutputProperties::DropOpenSort() {
  if (!before_open_sort) {
    return false;
  }
  arrangements = std::move(before_open_sort->arrangements);
  if (tree) {
    tree->walk = before_open_sort->walk;
  }
  before_open_sort.reset();
  return true;
}

void OutputProperties::CloseOpenSort() {
  before_open_sort.reset();
}

void OutputProperties::Group(const std::vector<ColumnId>& keys,
                             const std::vector<SortKey>& sorted_by) {
  CloseOpenSort();
  std::vector<Arrangement> kept;
  if (sorted_by.empty()) {
    // An order known within groups of a subset of `keys` still holds within
    // the new groups.
    for (const Arrangement& arrangement : arrangements) {
      if (std::all_of(
              arrangement.grouped_by.begin(), arrangement.grouped_by.end(),
              [&](ColumnId column) { return Contains(keys, column); })) {
        kept.push_back({keys, arrangement.sorted_by});
      }
    }
  }
  kept.push_back({keys, sorted_by});
  arrangements = std::move(kept);
  if (tree) {
    tree->walk.reset();
  }
  group_numbers.push_back({keys, column_count++});
}

void OutputProperties::WalkTree(ColumnId node,
                                ColumnId parent,
                                TreeDirection direction) {
  NumberTree(node, parent);
  CloseOpenSort();
  arrangements.clear();
  tree->walk = direction;
}

void OutputProperties::NumberTree(ColumnId node, ColumnId parent) {
  if (tree && tree->node == node && tree->parent == parent) {
    return;
  }
  uint32_t node_position = column_count++;
  uint32_t parent_position = column_count++;
  tree = Tree{node, parent, std::nullopt, node_position, parent_position};
}

uint32_t OutputProperties::AddColumn(ColumnId id) {
  positions[id] = column_count;
  return column_count++;
}

uint32_t OutputProperties::AddTemporaryColumn() {
  return column_count++;
}

void OutputProperties::Reset() {
  column_count = 0;
  positions.assign(positions.size(), kNoPosition);
  full_batches = true;
  arrangements.clear();
  tree.reset();
  group_numbers.clear();
  before_open_sort.reset();
}

void OutputProperties::DropRows() {
  CloseOpenSort();
  full_batches = false;
  tree.reset();
}

OutputProperties OutputProperties::Empty(size_t plan_columns) {
  OutputProperties empty;
  empty.positions.assign(plan_columns, kNoPosition);
  return empty;
}

OutputProperties::RequiredSteps OutputProperties::RequireSorted(
    const std::vector<SortKey>& keys) {
  RequiredSteps steps;
  if (IsArranged({}, keys)) {
    return steps;
  }
  steps.drop_open_sort = DropOpenSort();
  if (steps.drop_open_sort && IsArranged({}, keys)) {
    return steps;
  }
  steps.combine_batches = !full_batches;
  steps.reorder = true;
  full_batches = true;
  Sort(keys);
  return steps;
}

OutputProperties::RequiredSteps OutputProperties::RequireTreeWalk(
    ColumnId node,
    ColumnId parent,
    TreeDirection direction) {
  RequiredSteps steps;
  if (TreeWalk(node, parent) == direction) {
    return steps;
  }
  bool same_tree = tree && tree->node == node && tree->parent == parent;
  steps.drop_open_sort = same_tree && DropOpenSort();
  if (steps.drop_open_sort && TreeWalk(node, parent) == direction) {
    return steps;
  }
  bool up = direction == TreeDirection::kUp;
  steps.number_tree = !same_tree;
  // Walking up holds every row, and outputs full batches. Walking down outputs
  // rows as their parents arrive, so batches can be small.
  steps.combine_batches = up && !full_batches;
  steps.reorder = true;
  full_batches = up;
  WalkTree(node, parent, direction);
  return steps;
}

}  // namespace perfetto::trace_processor::pipeline
