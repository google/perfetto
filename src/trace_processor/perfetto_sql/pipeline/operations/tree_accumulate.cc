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

#include "src/trace_processor/perfetto_sql/pipeline/operations/tree_accumulate.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/core/exec/tree_accumulate.h"
#include "src/trace_processor/core/util/arena.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;

constexpr OperationRegistration TreeAccumulate::kRegistration{
    1, &MakeOperation<TreeAccumulate>};

std::optional<uint32_t> TreeAccumulate::Prune(std::vector<bool>* needed) {
  PruneAggregates(&aggregates_, needed);

  // A fold with nothing left to compute can go entirely. Checking that the
  // input is a valid tree is not something a fold promises on its own.
  if (aggregates_.empty()) {
    return 0;
  }

  // Otherwise it needs the tree's structure and the columns it aggregates.
  const auto& acc = *this;
  (*needed)[acc.node_column_] = true;
  (*needed)[acc.parent_column_] = true;
  return std::nullopt;
}

InputNeeds TreeAccumulate::Needs(uint32_t) const {
  InputNeeds needs;
  needs.tree_walk =
      InputNeeds::TreeWalk{node_column_, parent_column_, direction_};
  AddAggregateNeeds(aggregates_, &needs);
  return needs;
}

OutputProperties TreeAccumulate::Derive(DeriveContext& c) const {
  OutputProperties out = c.input(0);
  out.open_sort.reset();
  for (const Aggregate& agg : aggregates_) {
    out.columns.push_back(c.ColumnOf(agg.output));
  }
  return out;
}

bool TreeAccumulate::CommutesWith(const TreeAccumulate& below) const {
  if (direction_ == below.direction_ || node_column_ != below.node_column_ ||
      parent_column_ != below.parent_column_) {
    return false;
  }
  for (const Aggregate& agg : aggregates_) {
    for (const Aggregate& out : below.aggregates_) {
      if (agg.reads_column() && agg.column == out.output) {
        return false;
      }
    }
  }
  return true;
}

void TreeAccumulate::Rewrite(BoundPlan* bound,
                             PlanNodeId self,
                             const LoweringContext& context) const {
  std::vector<PlanNode>& nodes = bound->plan.nodes();
  PlanNode& node = nodes[self];
  PlanNodeId below_id = node.children()[0];
  PlanNode& below = nodes[below_id];
  if (!below.Is<TreeAccumulate>() ||
      !CommutesWith(below.Cast<TreeAccumulate>())) {
    return;
  }
  // If the rows already walk the tree, the fold in that direction goes first
  // and reuses the walk.
  std::optional<TreeDirection> walk =
      bound->planned[below.children()[0]].output.TreeWalk(node_column_,
                                                          parent_column_);
  if (walk && below.Cast<TreeAccumulate>().direction_ != *walk) {
    node.SwapOperation(&below);
    PlanNodeInputs(bound, below_id, context);
  }
}

void TreeAccumulate::Lower(Lowering* c, const PlanNode& node) const {
  const auto& acc = *this;
  c->LowerInput(node, 0);
  const OutputProperties::Tree& tree = *c->QueryRows().tree;
  // One operator folds every aggregate, appending their columns in order.
  core::Arena& arena = c->arena();
  ex::TreeAccumulateSpec spec{c->QueryPosition(tree.node_number),
                              c->QueryPosition(tree.parent_number),
                              LowerAggregates(c, acc.aggregates_)};
  if (acc.direction_ == TreeDirection::kUp) {
    c->AddOperator(arena.New<ex::TreeAccumulateUp>(arena, spec));
  } else {
    c->AddOperator(arena.New<ex::TreeAccumulateDown>(arena, spec));
  }
}

const OperationRegistration& TreeAccumulate::registration() const {
  return kRegistration;
}

void TreeAccumulate::Serialize(PlanCodec& c) {
  c.Pod(&direction_);
  c.Pod(&node_column_);
  c.Pod(&parent_column_);
  SerializeAggregates(c, &aggregates_);
}

}  // namespace perfetto::trace_processor::pipeline
