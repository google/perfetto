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
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
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

ColumnId NewPlanColumn(BoundPlan* bound, PlanNodeId node) {
  PlannedNode& planned = bound->planned[node];
  if (planned.new_columns_used < planned.new_columns.size()) {
    return planned.new_columns[planned.new_columns_used++];
  }
  LogicalPlan& plan = bound->plan;
  auto id = static_cast<ColumnId>(plan.columns().size());
  plan.columns().push_back({plan.names().Intern("$number"), PlanType::Null()});
  planned.new_columns.push_back(id);
  planned.new_columns_used++;
  return id;
}

void Fail(BoundPlan* bound, base::Status status) {
  if (bound->status.ok()) {
    bound->status = std::move(status);
  }
}

// Works out how one input of a node meets the node's needs.
class InputPlanner {
 public:
  InputPlanner(BoundPlan* bound,
               PlanNodeId node,
               uint32_t input,
               const OutputProperties& rows)
      : bound_(bound), node_(node), input_(input) {
    out_.rows = rows;
  }

  const OutputProperties& rows() const { return out_.rows; }

  void Convert(ColumnId column, const PlanType& type) {
    const PlanType& from = out_.rows.TypeOf(column);
    bool all_null = out_.rows.columns[*out_.rows.PositionOf(column)].all_null;
    if (!type.storage() ||
        (from.storage() && *from.storage() == *type.storage())) {
      return;
    }
    out_.steps.push_back(
        input_step::Convert{column, *type.storage(), all_null});
    out_.rows.SetType(column, *type.storage());
  }

  void Meet(const InputNeeds& needs) {
    Types(needs.types_first);
    if (needs.arranged) {
      Arrange(needs.arranged->grouped_by, needs.arranged->sorted_by);
      if (needs.group_numbers && !needs.arranged->grouped_by.empty() &&
          !out_.rows.GroupNumbersOf(needs.arranged->grouped_by)) {
        // The rows are grouped but have no group numbers. Grouping them again
        // adds the numbers.
        Group(needs.arranged->grouped_by, {});
      }
    }
    if (needs.tree_walk) {
      WalkTree(*needs.tree_walk);
    }
    Types(needs.types_then);
    if (needs.full_batches && !out_.rows.full_batches) {
      out_.steps.push_back(input_step::CombineBatches{});
      out_.rows.full_batches = true;
    }
  }

  PlannedInput Take() { return std::move(out_); }

 private:
  void Types(const std::vector<InputNeeds::Type>& types) {
    for (const InputNeeds::Type& need : types) {
      if (!need.if_unknown || !out_.rows.TypeOf(need.column).storage()) {
        RequireType(need.column, need.type);
      }
    }
  }

  void RequireType(ColumnId column, core::StorageType type) {
    std::optional<PlanType> common =
        CommonType(out_.rows.TypeOf(column), PlanType(type));
    if (!common || !common->storage() || !(*common->storage() == type)) {
      const char* what = type.Is<core::String>()   ? "strings"
                         : type.Is<core::Double>() ? "numbers"
                                                   : "integers";
      const LogicalPlan& plan = bound_->plan;
      Fail(bound_,
           base::ErrStatus(
               "column '%s' must hold %s",
               plan.names().text(plan.columns()[column].name).c_str(), what));
      return;
    }
    Convert(column, type);
  }

  // A sort key of unknown type must hold integers.
  void RequireSortKeyTypes(const std::vector<SortKey>& keys) {
    for (const SortKey& key : keys) {
      if (!out_.rows.TypeOf(key.column).storage()) {
        RequireType(key.column, core::Int64{});
      }
    }
  }

  void Arrange(const std::vector<ColumnId>& keys,
               const std::vector<SortKey>& sorted_by) {
    OutputProperties& rows = out_.rows;
    if (rows.IsArranged(keys, sorted_by) ||
        (DropOpenSort() && rows.IsArranged(keys, sorted_by))) {
      return;
    }
    if (keys.empty()) {
      Sort(sorted_by);
      return;
    }
    // Grouping is stable, so if the rows are already sorted by `sorted_by`,
    // grouping them is enough.
    Group(keys,
          rows.IsArranged({}, sorted_by) ? std::vector<SortKey>() : sorted_by);
  }

  bool DropOpenSort() {
    OutputProperties& rows = out_.rows;
    if (!rows.open_sort) {
      return false;
    }
    OutputProperties::OpenSort sort = std::move(*rows.open_sort);
    rows.open_sort.reset();
    std::vector<InputStep>& steps =
        bound_->planned[sort.node].inputs[sort.input].steps;
    // Batches were combined for the sort, which what was planned since assumes.
    if (std::get<input_step::Sort>(steps[sort.step]).combine_first) {
      steps[sort.step] = input_step::CombineBatches{};
    } else {
      steps.erase(steps.begin() + static_cast<ptrdiff_t>(sort.step));
    }
    rows.arrangements = std::move(sort.arrangements);
    if (rows.tree) {
      rows.tree->walk = sort.walk;
    }
    return true;
  }

  void Sort(const std::vector<SortKey>& keys) {
    RequireSortKeyTypes(keys);
    OutputProperties& rows = out_.rows;
    OutputProperties::OpenSort sort;
    sort.node = node_;
    sort.input = input_;
    sort.step = out_.steps.size();
    sort.arrangements = std::move(rows.arrangements);
    if (rows.tree) {
      sort.walk = rows.tree->walk;
      rows.tree->walk.reset();
    }
    out_.steps.push_back(input_step::Sort{keys, !rows.full_batches});
    rows.full_batches = true;
    rows.arrangements = {{{}, keys}};
    rows.open_sort = std::move(sort);
  }

  void Group(const std::vector<ColumnId>& keys,
             const std::vector<SortKey>& sorted_by) {
    RequireSortKeyTypes(sorted_by);
    OutputProperties& rows = out_.rows;
    ColumnId numbers = NewPlanColumn(bound_, node_);
    out_.steps.push_back(
        input_step::Group{keys, sorted_by, numbers, !rows.full_batches});
    rows.columns.push_back({numbers, PlanType::Null()});
    rows.full_batches = true;
    rows.open_sort.reset();
    std::vector<OutputProperties::Arrangement> kept;
    if (sorted_by.empty()) {
      // An order known within groups of a subset of `keys` still holds within
      // the new groups.
      for (const OutputProperties::Arrangement& arrangement :
           rows.arrangements) {
        if (std::all_of(
                arrangement.grouped_by.begin(), arrangement.grouped_by.end(),
                [&](ColumnId column) { return Contains(keys, column); })) {
          kept.push_back({keys, arrangement.sorted_by});
        }
      }
    }
    kept.push_back({keys, sorted_by});
    rows.arrangements = std::move(kept);
    if (rows.tree) {
      rows.tree->walk.reset();
    }
    rows.group_numbers.push_back({keys, numbers});
  }

  void WalkTree(const InputNeeds::TreeWalk& walk) {
    OutputProperties& rows = out_.rows;
    if (rows.TreeWalk(walk.node, walk.parent) == walk.direction) {
      return;
    }
    bool same_tree = rows.tree && rows.tree->node == walk.node &&
                     rows.tree->parent == walk.parent;
    if (same_tree && DropOpenSort() &&
        rows.TreeWalk(walk.node, walk.parent) == walk.direction) {
      return;
    }
    if (!same_tree) {
      ColumnId node_number = NewPlanColumn(bound_, node_);
      ColumnId parent_number = NewPlanColumn(bound_, node_);
      out_.steps.push_back(input_step::NumberTree{walk.node, walk.parent,
                                                  node_number, parent_number});
      rows.columns.push_back({node_number, PlanType::Null()});
      rows.columns.push_back({parent_number, PlanType::Null()});
      rows.tree = OutputProperties::Tree{walk.node, walk.parent, std::nullopt,
                                         node_number, parent_number};
    }
    // Walking up holds every row, and outputs full batches. Walking down
    // outputs rows as their parents arrive, so batches can be small.
    bool up = walk.direction == TreeDirection::kUp;
    out_.steps.push_back(
        input_step::WalkTree{rows.tree->node_number, rows.tree->parent_number,
                             walk.direction, up && !rows.full_batches});
    rows.full_batches = up;
    rows.open_sort.reset();
    rows.arrangements.clear();
    rows.tree->walk = walk.direction;
  }

  BoundPlan* bound_;
  PlanNodeId node_;
  uint32_t input_;
  PlannedInput out_;
};

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
    if (sorted.size() - next >= wanted.size() &&
        std::equal(wanted.begin(), wanted.end(),
                   sorted.begin() + static_cast<ptrdiff_t>(next),
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
    const std::vector<ColumnId>& allowed) const {
  std::vector<SortKey> longest;
  for (const Arrangement& arrangement : arrangements) {
    if (!arrangement.grouped_by.empty()) {
      continue;
    }
    std::vector<SortKey> keys;
    for (const SortKey& key : arrangement.sorted_by) {
      if (!Contains(allowed, key.column)) {
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

std::optional<uint32_t> OutputProperties::PositionOf(ColumnId column) const {
  auto it = std::find_if(columns.begin(), columns.end(),
                         [&](const Column& c) { return c.id == column; });
  if (it == columns.end()) {
    return std::nullopt;
  }
  return static_cast<uint32_t>(it - columns.begin());
}

const PlanType& OutputProperties::TypeOf(ColumnId column) const {
  std::optional<uint32_t> position = PositionOf(column);
  PERFETTO_CHECK(position);
  return columns[*position].type;
}

void OutputProperties::SetType(ColumnId column, core::StorageType type) {
  std::optional<uint32_t> position = PositionOf(column);
  PERFETTO_CHECK(position);
  columns[*position].type = type;
}

std::optional<ColumnId> OutputProperties::GroupNumbersOf(
    const std::vector<ColumnId>& keys) const {
  for (const GroupNumbers& numbers : group_numbers) {
    if (std::is_permutation(numbers.keys.begin(), numbers.keys.end(),
                            keys.begin(), keys.end())) {
      return numbers.column;
    }
  }
  return std::nullopt;
}

void OutputProperties::DropRows() {
  open_sort.reset();
  full_batches = false;
  tree.reset();
}

const OutputProperties& DeriveContext::input(uint32_t i) const {
  return bound_->planned[self_].inputs[i].rows;
}

OutputProperties::Column DeriveContext::ColumnOf(ColumnId id) const {
  for (const PlannedInput& input : bound_->planned[self_].inputs) {
    if (input.rows.PositionOf(id)) {
      return input.rows.columns[*input.rows.PositionOf(id)];
    }
  }
  const PlanType& type = bound_->bindings.types[id];
  return {id, type, type.is_null()};
}

OutputProperties::Column DeriveContext::NewColumn() {
  return {NewPlanColumn(bound_, self_), PlanType::Null()};
}

void DeriveContext::SetPrograms(std::vector<core::exec::ExprProgram> programs) {
  bound_->planned[self_].programs = std::move(programs);
}

std::vector<std::optional<core::exec::FilterValue>>&
DeriveContext::filter_values() {
  return bound_->planned[self_].filter_values;
}

void DeriveContext::Fail(base::Status status) {
  pipeline::Fail(bound_, std::move(status));
}

InputNeeds IntervalLaneNeeds(ColumnId ts,
                             ColumnId dur,
                             const std::vector<ColumnId>& keys) {
  InputNeeds needs;
  needs.types_first.push_back({ts, core::Int64{}, false});
  needs.types_first.push_back({dur, core::Int64{}, false});
  needs.arranged = OutputProperties::Arrangement{keys, {{ts, false}}};
  needs.group_numbers = true;
  return needs;
}

void PlanNodeInputs(BoundPlan* bound,
                    PlanNodeId id,
                    const LoweringContext& lowering) {
  bound->planned[id].new_columns_used = 0;
  const PlanNode& node = bound->plan.nodes()[id];
  const PlanOperation& op = node.operation();
  std::vector<InputPlanner> planners;
  for (uint32_t i = 0; i < node.children().size(); ++i) {
    planners.emplace_back(bound, id, i,
                          bound->planned[node.children()[i]].output);
  }
  for (const std::vector<InputColumn>& same : op.CommonTypes()) {
    PlanType common = PlanType::Null();
    for (const InputColumn& side : same) {
      std::optional<PlanType> next =
          CommonType(common, planners[side.input].rows().TypeOf(side.column));
      if (!next) {
        const LogicalPlan& plan = bound->plan;
        Fail(
            bound,
            base::ErrStatus(
                "column '%s' holds strings in one input and numbers in another",
                plan.names().text(plan.columns()[side.column].name).c_str()));
        return;
      }
      common = *next;
    }
    for (const InputColumn& side : same) {
      planners[side.input].Convert(side.column, common);
    }
  }
  std::vector<PlannedInput> inputs;
  for (uint32_t i = 0; i < planners.size(); ++i) {
    planners[i].Meet(op.Needs(i));
    inputs.push_back(planners[i].Take());
  }
  bound->planned[id].inputs = std::move(inputs);
  DeriveContext context(bound, id, lowering);
  OutputProperties output = op.Derive(context);
  bound->planned[id].output = std::move(output);
}

}  // namespace perfetto::trace_processor::pipeline
