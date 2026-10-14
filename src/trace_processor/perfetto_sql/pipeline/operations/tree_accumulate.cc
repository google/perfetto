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
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;

const OperationRegistration TreeAccumulate::kRegistration{
    SYNTAQLITE_NODE_PERFETTO_TREE_ACCUMULATE, &BuildPlan,
    OperationRegistration::Encoding{1, false, &DecodePlan}};

base::Status TreeAccumulate::BuildPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("TREE ACCUMULATE");
  const auto* n = Node<SyntaqlitePerfettoTreeAccumulate>(c->parser(), stage);

  TreeAccumulate acc;
  acc.direction_ = n->direction == SYNTAQLITE_PERFETTO_TREE_DIRECTION_DOWN
                       ? TreeDirection::kDown
                       : TreeDirection::kUp;
  ASSIGN_OR_RETURN(acc.node_column_, c->Resolve("id", stage));
  ASSIGN_OR_RETURN(acc.parent_column_, c->Resolve("parent_id", stage));

  std::vector<std::pair<NamedColumn, uint32_t>> output;
  const auto* list =
      Node<SyntaqlitePerfettoAggregateList>(c->parser(), n->aggregates);
  uint32_t count = syntaqlite_list_count(list);
  for (uint32_t i = 0; i < count; i++) {
    uint32_t agg_id = syntaqlite_list_child_id(list, i);
    const auto* agg = Node<SyntaqlitePerfettoAggregate>(c->parser(), agg_id);
    ASSIGN_OR_RETURN(Aggregate aggregate, c->ResolveAggregate(agg->expr));
    std::string name = SpanText(c->parser(), agg->name);
    aggregate.output = c->AddColumn(name, core::Int64{});
    acc.aggregates_.push_back(aggregate);
    output.push_back({NamedColumn{std::move(name), aggregate.output}, agg_id});
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

void TreeAccumulate::Lower(Lowering* c, const PlanNode& node) const {
  const auto& acc = *this;
  c->LowerNode(node.children()[0]);

  const auto tree =
      c->PrepareTree(acc.node_column_, acc.parent_column_, acc.direction_);
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
    c->Define(agg.output);
  }
}

const OperationRegistration& TreeAccumulate::registration() const {
  return kRegistration;
}

std::unique_ptr<PlanOperation> TreeAccumulate::Clone() const {
  return std::make_unique<TreeAccumulate>(*this);
}

void TreeAccumulate::Write(PlanWriter* c,
                           const PlanNode&,
                           Available* available_columns) const {
  auto& available = *available_columns;
  const auto& acc = *this;
  c->writer().U8(static_cast<uint8_t>(acc.direction_));
  c->writer().Position(available, acc.node_column_);
  c->writer().Position(available, acc.parent_column_);
  WriteAggregates(c, acc.aggregates_, available);
  for (const Aggregate& agg : acc.aggregates_) {
    available.push_back(agg.output);
  }
}

void TreeAccumulate::DecodePlan(PlanReader* c, Available* available_columns) {
  auto& available = *available_columns;
  TreeAccumulate acc;
  acc.direction_ = static_cast<TreeDirection>(c->reader().U8());
  acc.node_column_ = c->reader().Position(available);
  acc.parent_column_ = c->reader().Position(available);
  ReadAggregates(c, available, &acc.aggregates_);
  for (const Aggregate& agg : acc.aggregates_) {
    available.push_back(agg.output);
  }
  c->AddNode(std::move(acc), {c->plan().root()});
}

}  // namespace perfetto::trace_processor::pipeline
