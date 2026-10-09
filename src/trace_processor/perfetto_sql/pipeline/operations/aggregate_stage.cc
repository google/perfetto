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

#include "src/trace_processor/perfetto_sql/pipeline/operations/aggregate_stage.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/hash_aggregate.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;

const OperationRegistration AggregateStage::kRegistration{
    SYNTAQLITE_NODE_PERFETTO_PIPE_AGGREGATE, &BuildPlan,
    OperationRegistration::Encoding{5, false, &DecodePlan}};

base::Status AggregateStage::BuildPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("AGGREGATE");
  const auto* n = Node<SyntaqlitePerfettoPipeAggregate>(c->parser(), stage);

  AggregateStage aggregate;
  // Every column out, the keys then the aggregates, is named once.
  std::vector<std::string> names;
  std::vector<std::pair<NamedColumn, uint32_t>> keys;
  if (syntaqlite_node_is_present(n->group_by)) {
    const auto* list =
        Node<SyntaqlitePerfettoPipeColumnList>(c->parser(), n->group_by);
    for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
      uint32_t item_id = syntaqlite_list_child_id(list, i);
      const auto* item =
          Node<SyntaqlitePerfettoPipeColumn>(c->parser(), item_id);
      std::string qualifier = IsSpanPresent(item->qualifier)
                                  ? SpanText(c->parser(), item->qualifier)
                                  : "";
      std::string name = SpanText(c->parser(), item->name);
      ASSIGN_OR_RETURN(ColumnId key, c->Resolve(qualifier, name, item_id));
      if (IsSpanPresent(item->alias)) {
        name = SpanText(c->parser(), item->alias);
      }
      RETURN_IF_ERROR(c->CheckListedOnce(names, name, item_id));
      aggregate.keys_.push_back(key);
      keys.push_back({NamedColumn{std::move(name), key}, item_id});
    }
  }

  std::vector<std::pair<NamedColumn, uint32_t>> aggregates;
  const auto* list =
      Node<SyntaqlitePerfettoAggregateList>(c->parser(), n->aggregates);
  for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
    uint32_t agg_id = syntaqlite_list_child_id(list, i);
    const auto* agg = Node<SyntaqlitePerfettoAggregate>(c->parser(), agg_id);
    ASSIGN_OR_RETURN(Aggregate resolved, c->ResolveAggregate(agg->expr));
    std::string name = SpanText(c->parser(), agg->name);
    RETURN_IF_ERROR(c->CheckListedOnce(names, name, agg_id));
    resolved.output = c->AddColumn(name, core::Int64{});
    aggregate.aggregates_.push_back(resolved);
    aggregates.push_back(
        {NamedColumn{std::move(name), resolved.output}, agg_id});
  }

  // The rows collapse, so the row is only what each group holds.
  c->ReplaceRow({});
  c->ClearAliases();
  for (auto& [column, node] : keys) {
    c->Append(std::move(column), node);
  }
  for (auto& [column, node] : aggregates) {
    c->Append(std::move(column), node);
  }
  c->AddNode(std::move(aggregate), {c->plan().root()});
  return base::OkStatus();
}

std::optional<uint32_t> AggregateStage::Prune(std::vector<bool>* needed) {
  PruneAggregates(&aggregates_, needed);
  // The keys make the groups, so they stay even when nothing reads them.
  for (ColumnId key : keys_) {
    (*needed)[key] = true;
  }
  return std::nullopt;
}

void AggregateStage::Lower(Lowering* c, const PlanNode& node) const {
  c->LowerNode(node.children()[0]);
  ex::HashAggregateSpec spec;
  for (ColumnId key : keys_) {
    spec.key_columns.push_back(c->Position(key));
  }
  for (const Aggregate& agg : aggregates_) {
    spec.aggregates.push_back(LowerAggregate(c, agg));
  }
  c->AddOperator(std::make_unique<ex::HashAggregate>(std::move(spec)));

  // Its rows are the groups, laid out afresh, in no particular order.
  c->ResetLayout();
  for (ColumnId key : keys_) {
    c->Define(key);
  }
  for (const Aggregate& agg : aggregates_) {
    c->Define(agg.output);
  }
  c->SetOrder({});
}

const OperationRegistration& AggregateStage::registration() const {
  return kRegistration;
}

std::unique_ptr<PlanOperation> AggregateStage::Clone() const {
  return std::make_unique<AggregateStage>(*this);
}

// Replaces `available` with the groups' columns.
void AggregateStage::Write(PlanWriter* c,
                           const PlanNode&,
                           Available* available_columns) const {
  auto& available = *available_columns;
  c->writer().Size(keys_.size());
  for (ColumnId key : keys_) {
    c->writer().Position(available, key);
  }
  WriteAggregates(c, aggregates_, available);
  available = keys_;
  for (const Aggregate& agg : aggregates_) {
    available.push_back(agg.output);
  }
}

void AggregateStage::DecodePlan(PlanReader* c, Available* available_columns) {
  auto& available = *available_columns;
  AggregateStage aggregate;
  aggregate.keys_.resize(c->reader().Count());
  for (ColumnId& key : aggregate.keys_) {
    key = c->reader().Position(available);
  }
  ReadAggregates(c, available, &aggregate.aggregates_);
  available = aggregate.keys_;
  for (const Aggregate& agg : aggregate.aggregates_) {
    available.push_back(agg.output);
  }
  c->AddNode(std::move(aggregate), {c->plan().root()});
}

}  // namespace perfetto::trace_processor::pipeline
