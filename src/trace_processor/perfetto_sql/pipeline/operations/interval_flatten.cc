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

#include "src/trace_processor/perfetto_sql/pipeline/operations/interval_flatten.h"

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
#include "src/trace_processor/core/exec/interval_flatten.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;

const OperationRegistration IntervalFlatten::kRegistration{
    SYNTAQLITE_NODE_PERFETTO_INTERVAL_FLATTEN, &BuildPlan};

base::Status IntervalFlatten::BuildPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("INTERVAL FLATTEN");
  const auto* n = Node<SyntaqlitePerfettoIntervalFlatten>(c->parser(), stage);

  IntervalFlatten flatten;
  ASSIGN_OR_RETURN(flatten.ts_, c->Resolve("ts", stage));
  ASSIGN_OR_RETURN(flatten.dur_, c->Resolve("dur", stage));

  std::vector<std::pair<NamedColumn, uint32_t>> keys;
  if (syntaqlite_node_is_present(n->per)) {
    const auto* per =
        Node<SyntaqlitePerfettoPerColumnList>(c->parser(), n->per);
    std::vector<std::string> seen;
    for (uint32_t i = 0; i < syntaqlite_list_count(per); ++i) {
      uint32_t col_id = syntaqlite_list_child_id(per, i);
      std::string name = SpanText(
          c->parser(),
          Node<SyntaqlitePerfettoPerColumn>(c->parser(), col_id)->name);
      RETURN_IF_ERROR(c->CheckListedOnce(seen, name, col_id));
      ASSIGN_OR_RETURN(ColumnId key, c->Resolve(name, col_id));
      flatten.keys_.push_back(key);
      keys.push_back({NamedColumn{std::move(name), key}, col_id});
    }
  }

  std::vector<std::pair<NamedColumn, uint32_t>> aggregates;
  const auto* list =
      Node<SyntaqlitePerfettoAggregateList>(c->parser(), n->aggregates);
  for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
    uint32_t agg_id = syntaqlite_list_child_id(list, i);
    const auto* agg = Node<SyntaqlitePerfettoAggregate>(c->parser(), agg_id);
    IntervalFlatten::Aggregate aggregate;
    if (c->IsCountStar(agg->expr)) {
      aggregate.function = IntervalFlatten::Function::kCount;
    } else {
      aggregate.function = IntervalFlatten::Function::kSum;
      ASSIGN_OR_RETURN(aggregate.column, c->ResolveSum(agg_id, agg->expr));
    }
    std::string name = SpanText(c->parser(), agg->name);
    aggregate.output = c->AddColumn(name, core::Int64{});
    flatten.aggregates_.push_back(aggregate);
    aggregates.push_back(
        {NamedColumn{std::move(name), aggregate.output}, agg_id});
  }

  // The rows collapse, so the row is only what each segment holds.
  flatten.out_ts_ = c->AddColumn("ts", core::Int64{});
  flatten.out_dur_ = c->AddColumn("dur", core::Int64{});
  c->ReplaceRow({});
  c->ClearAliases();
  c->Append({"ts", flatten.out_ts_}, stage);
  c->Append({"dur", flatten.out_dur_}, stage);
  for (auto& [column, node] : keys) {
    c->Append(std::move(column), node);
  }
  for (auto& [column, node] : aggregates) {
    c->Append(std::move(column), node);
  }
  c->AddNode(std::move(flatten), {c->plan().root()});
  return base::OkStatus();
}

std::optional<uint32_t> IntervalFlatten::Prune(std::vector<bool>* used) {
  auto& needed = *used;
  auto& flatten = *this;
  aggregates_.erase(std::remove_if(aggregates_.begin(), aggregates_.end(),
                                   [&](const IntervalFlatten::Aggregate& agg) {
                                     return !needed[agg.output];
                                   }),
                    aggregates_.end());
  // Unlike a fold, it stays with no aggregates left: its rows are segments.
  needed[flatten.ts_] = true;
  needed[flatten.dur_] = true;
  for (ColumnId key : flatten.keys_) {
    needed[key] = true;
  }
  for (const IntervalFlatten::Aggregate& agg : aggregates_) {
    if (agg.function == IntervalFlatten::Function::kSum) {
      needed[agg.column] = true;
    }
  }
  return std::nullopt;
}

void IntervalFlatten::Lower(Lowering* c, const PlanNode& node) const {
  const auto& flatten = *this;
  c->LowerNode(node.children()[0]);

  c->RequireInt64(flatten.ts_);
  c->RequireInt64(flatten.dur_);
  c->PrepareGroups(flatten.keys_, flatten.ts_);
  ex::IntervalFlattenSpec spec;
  spec.ts_column = c->Position(flatten.ts_);
  spec.dur_column = c->Position(flatten.dur_);
  for (ColumnId key : flatten.keys_) {
    spec.key_columns.push_back(c->Position(key));
  }
  spec.group_column = c->order().group_column;
  for (const IntervalFlatten::Aggregate& agg : flatten.aggregates_) {
    ex::IntervalFlattenSpec::Aggregate lowered;
    switch (agg.function) {
      case IntervalFlatten::Function::kCount:
        lowered.function = ex::IntervalFlattenSpec::Function::kCount;
        break;
      case IntervalFlatten::Function::kSum:
        c->RequireInt64(agg.column);
        lowered.function = ex::IntervalFlattenSpec::Function::kSum;
        lowered.column = c->Position(agg.column);
        break;
    }
    spec.aggregates.push_back(lowered);
  }
  c->AddOperator(std::make_unique<ex::IntervalFlatten>(std::move(spec)));

  // Its rows are the segments, laid out afresh.
  c->ResetLayout();
  c->Define(flatten.out_ts_);
  c->Define(flatten.out_dur_);
  for (ColumnId key : flatten.keys_) {
    c->Define(key);
  }
  for (const IntervalFlatten::Aggregate& agg : flatten.aggregates_) {
    c->Define(agg.output);
  }
  c->SetOrder({flatten.keys_, c->AllocateTemporaryColumn(), {flatten.out_ts_}});
}

}  // namespace perfetto::trace_processor::pipeline
