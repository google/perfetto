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
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/tree_accumulate.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;

const OperationRegistration TreeAccumulate::kRegistration{
    SYNTAQLITE_NODE_PERFETTO_TREE_ACCUMULATE, &BuildPlan,
    OperationRegistration::Encoding{1, &MakeOperation<TreeAccumulate>}};

base::Status TreeAccumulate::BuildPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("TREE ACCUMULATE");
  const auto* n = Node<SyntaqlitePerfettoTreeAccumulate>(c->parser(), stage);

  TreeAccumulate acc;
  acc.direction_ = n->direction == SYNTAQLITE_PERFETTO_TREE_DIRECTION_DOWN
                       ? TreeDirection::kDown
                       : TreeDirection::kUp;
  ASSIGN_OR_RETURN(acc.node_column_, c->Resolve(c->Name("id"), stage));
  ASSIGN_OR_RETURN(acc.parent_column_, c->Resolve(c->Name("parent_id"), stage));

  std::vector<std::pair<NamedColumn, uint32_t>> output;
  const auto* list =
      Node<SyntaqlitePerfettoAggregateList>(c->parser(), n->aggregates);
  uint32_t count = syntaqlite_list_count(list);
  for (uint32_t i = 0; i < count; i++) {
    uint32_t agg_id = syntaqlite_list_child_id(list, i);
    const auto* agg = Node<SyntaqlitePerfettoAggregate>(c->parser(), agg_id);
    ASSIGN_OR_RETURN(Aggregate aggregate, c->ResolveAggregate(agg->expr));
    NameId name = c->Name(agg->name);
    aggregate.output = c->AddColumn(name, core::Int64{});
    acc.aggregates_.push_back(aggregate);
    output.push_back({NamedColumn{name, aggregate.output}, agg_id});
  }
  // All expressions see the input scope. Add this stage's columns only after
  // resolving every aggregate; duplicate names are ambiguous on lookup.
  for (auto& [column, node] : output) {
    c->Append(std::move(column), node);
  }
  c->AddNode(std::move(acc), {c->plan().root()});
  return base::OkStatus();
}

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

void ReorderTreeFolds(BoundPlan* plan, const LoweringContext& context) {
  // A node's inputs come before it, so each fold sees its input reordered.
  std::vector<PlanNode>& nodes = plan->plan.nodes();
  for (PlanNode& node : nodes) {
    if (!node.Is<TreeAccumulate>()) {
      continue;
    }
    PlanNode& below = nodes[node.children()[0]];
    const auto& fold = node.Cast<TreeAccumulate>();
    if (!below.Is<TreeAccumulate>() ||
        !fold.CommutesWith(below.Cast<TreeAccumulate>())) {
      continue;
    }
    // If the rows already walk the tree, the fold in that direction goes
    // first and reuses the walk.
    PlanLowering input(*plan, context);
    input.main().LowerNode(below.children()[0]);
    if (!input.status().ok()) {
      continue;
    }
    std::optional<TreeDirection> walk =
        input.main().QueryTreeWalk(fold.node_column_, fold.parent_column_);
    if (walk && below.Cast<TreeAccumulate>().direction_ != *walk) {
      node.SwapOperation(&below);
    }
  }
}

void TreeAccumulate::Lower(Lowering* c, const PlanNode& node) const {
  const auto& acc = *this;
  c->LowerNode(node.children()[0]);

  const auto tree =
      c->RequireTreeWalk(acc.node_column_, acc.parent_column_, acc.direction_);
  // One operator folds every aggregate, appending their columns in order.
  ex::TreeAccumulateSpec spec{tree.node, tree.parent, {}};
  for (const Aggregate& agg : acc.aggregates_) {
    spec.aggregates.push_back(LowerAggregate(c, agg));
  }
  if (acc.direction_ == TreeDirection::kUp) {
    c->AddOperator(std::make_unique<ex::TreeAccumulateUp>(std::move(spec)));
  } else {
    c->AddOperator(std::make_unique<ex::TreeAccumulateDown>(std::move(spec)));
  }
  for (const Aggregate& agg : acc.aggregates_) {
    c->DeclareColumn(agg.output);
  }
}

const OperationRegistration& TreeAccumulate::registration() const {
  return kRegistration;
}

std::unique_ptr<PlanOperation> TreeAccumulate::Clone() const {
  return std::make_unique<TreeAccumulate>(*this);
}

void TreeAccumulate::Serialize(PlanCodec& c) {
  c.Pod(&direction_);
  c.Pod(&node_column_);
  c.Pod(&parent_column_);
  SerializeAggregates(c, &aggregates_);
}

}  // namespace perfetto::trace_processor::pipeline
