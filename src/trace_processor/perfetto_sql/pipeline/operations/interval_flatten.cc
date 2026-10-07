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
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;

const OperationRegistration IntervalFlatten::kRegistration{
    SYNTAQLITE_NODE_PERFETTO_INTERVAL_FLATTEN, &BuildPlan,
    OperationRegistration::Encoding{3, false, &DecodePlan}};

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

std::optional<uint32_t> IntervalFlatten::Prune(std::vector<bool>* needed) {
  auto& flatten = *this;
  aggregates_.erase(std::remove_if(aggregates_.begin(), aggregates_.end(),
                                   [&](const IntervalFlatten::Aggregate& agg) {
                                     return !(*needed)[agg.output];
                                   }),
                    aggregates_.end());
  // Unlike a fold, it stays with no aggregates left: its rows are segments.
  (*needed)[flatten.ts_] = true;
  (*needed)[flatten.dur_] = true;
  for (ColumnId key : flatten.keys_) {
    (*needed)[key] = true;
  }
  for (const IntervalFlatten::Aggregate& agg : aggregates_) {
    if (agg.function == IntervalFlatten::Function::kSum) {
      (*needed)[agg.column] = true;
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

const OperationRegistration& IntervalFlatten::registration() const {
  return kRegistration;
}

std::unique_ptr<PlanOperation> IntervalFlatten::Clone() const {
  return std::make_unique<IntervalFlatten>(*this);
}

// Replaces `available` with the segment's columns.
void IntervalFlatten::Write(PlanWriter* c,
                            const PlanNode&,
                            Available* available_columns) const {
  auto& available = *available_columns;
  const auto& flatten = *this;
  c->writer().Position(available, flatten.ts_);
  c->writer().Position(available, flatten.dur_);
  c->writer().Size(flatten.keys_.size());
  for (ColumnId key : flatten.keys_) {
    c->writer().Position(available, key);
  }
  c->writer().Size(flatten.aggregates_.size());
  for (const IntervalFlatten::Aggregate& agg : flatten.aggregates_) {
    c->writer().U8(static_cast<uint8_t>(agg.function));
    if (agg.function == IntervalFlatten::Function::kSum) {
      c->writer().Position(available, agg.column);
    }
    c->writer().Str(c->plan().columns()[agg.output].name);
  }
  c->writer().Str(c->plan().columns()[flatten.out_ts_].name);
  c->writer().Str(c->plan().columns()[flatten.out_dur_].name);
  available = {flatten.out_ts_, flatten.out_dur_};
  available.insert(available.end(), flatten.keys_.begin(), flatten.keys_.end());
  for (const IntervalFlatten::Aggregate& agg : flatten.aggregates_) {
    available.push_back(agg.output);
  }
}

void IntervalFlatten::DecodePlan(PlanReader* c, Available* available_columns) {
  auto& available = *available_columns;
  IntervalFlatten flatten;
  flatten.ts_ = c->reader().Position(available);
  flatten.dur_ = c->reader().Position(available);
  flatten.keys_.resize(c->reader().Count());
  for (ColumnId& key : flatten.keys_) {
    key = c->reader().Position(available);
  }
  flatten.aggregates_.resize(c->reader().Count());
  for (IntervalFlatten::Aggregate& agg : flatten.aggregates_) {
    switch (c->reader().U8()) {
      case static_cast<uint8_t>(IntervalFlatten::Function::kCount):
        agg.function = IntervalFlatten::Function::kCount;
        break;
      case static_cast<uint8_t>(IntervalFlatten::Function::kSum):
        agg.function = IntervalFlatten::Function::kSum;
        agg.column = c->reader().Position(available);
        break;
      default:
        c->reader().Fail();
        break;
    }
    agg.output = c->AddColumn(c->reader().Str(), core::Int64{});
  }
  flatten.out_ts_ = c->AddColumn(c->reader().Str(), core::Int64{});
  flatten.out_dur_ = c->AddColumn(c->reader().Str(), core::Int64{});
  available = {flatten.out_ts_, flatten.out_dur_};
  available.insert(available.end(), flatten.keys_.begin(), flatten.keys_.end());
  for (const IntervalFlatten::Aggregate& agg : flatten.aggregates_) {
    available.push_back(agg.output);
  }
  c->AddNode(std::move(flatten), {c->plan().root()});
}

}  // namespace perfetto::trace_processor::pipeline
