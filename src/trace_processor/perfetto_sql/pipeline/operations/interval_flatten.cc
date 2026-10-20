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
    OperationRegistration::Encoding{3, false, &MakeOperation<IntervalFlatten>}};

base::Status IntervalFlatten::BuildPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("INTERVAL FLATTEN");
  const auto* n = Node<SyntaqlitePerfettoIntervalFlatten>(c->parser(), stage);

  IntervalFlatten flatten;
  NameId ts = c->Name("ts");
  NameId dur = c->Name("dur");
  ASSIGN_OR_RETURN(flatten.ts_, c->Resolve(ts, stage));
  ASSIGN_OR_RETURN(flatten.dur_, c->Resolve(dur, stage));

  std::vector<std::pair<NamedColumn, uint32_t>> keys;
  if (syntaqlite_node_is_present(n->per)) {
    const auto* per =
        Node<SyntaqlitePerfettoPerColumnList>(c->parser(), n->per);
    std::vector<NameId> seen;
    for (uint32_t i = 0; i < syntaqlite_list_count(per); ++i) {
      uint32_t col_id = syntaqlite_list_child_id(per, i);
      NameId name =
          c->Name(Node<SyntaqlitePerfettoPerColumn>(c->parser(), col_id)->name);
      RETURN_IF_ERROR(c->CheckListedOnce(seen, name, col_id));
      ASSIGN_OR_RETURN(ColumnId key, c->Resolve(name, col_id));
      flatten.keys_.push_back(key);
      keys.push_back({NamedColumn{name, key}, col_id});
    }
  }

  std::vector<std::pair<NamedColumn, uint32_t>> aggregates;
  const auto* list =
      Node<SyntaqlitePerfettoAggregateList>(c->parser(), n->aggregates);
  for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
    uint32_t agg_id = syntaqlite_list_child_id(list, i);
    const auto* agg = Node<SyntaqlitePerfettoAggregate>(c->parser(), agg_id);
    ASSIGN_OR_RETURN(Aggregate aggregate, c->ResolveAggregate(agg->expr));
    NameId name = c->Name(agg->name);
    aggregate.output = c->AddColumn(name, core::Int64{});
    flatten.aggregates_.push_back(aggregate);
    aggregates.push_back({NamedColumn{name, aggregate.output}, agg_id});
  }

  // The rows collapse, so the row is only what each segment holds.
  flatten.out_ts_ = c->AddColumn(ts, core::Int64{});
  flatten.out_dur_ = c->AddColumn(dur, core::Int64{});
  c->ReplaceRow({});
  c->ClearAliases();
  c->Append({ts, flatten.out_ts_}, stage);
  c->Append({dur, flatten.out_dur_}, stage);
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
  PruneAggregates(&aggregates_, needed);
  // Unlike a fold, it stays with no aggregates left: its rows are segments.
  (*needed)[flatten.ts_] = true;
  (*needed)[flatten.dur_] = true;
  for (ColumnId key : flatten.keys_) {
    (*needed)[key] = true;
  }
  return std::nullopt;
}

void IntervalFlatten::Lower(Lowering* c, const PlanNode& node) const {
  const auto& flatten = *this;
  c->LowerNode(node.children()[0]);

  ex::IntervalFlattenSpec spec;
  spec.lanes =
      c->RequireIntervalLanes(flatten.ts_, flatten.dur_, flatten.keys_);
  for (const Aggregate& agg : flatten.aggregates_) {
    spec.aggregates.push_back(LowerAggregate(c, agg));
  }
  c->RequireFullBatches();
  c->AddOperator(std::make_unique<ex::IntervalFlatten>(std::move(spec)));

  // Its rows are the segments, laid out afresh.
  c->ResetLayout();
  c->DeclareColumn(flatten.out_ts_);
  c->DeclareColumn(flatten.out_dur_);
  for (ColumnId key : flatten.keys_) {
    c->DeclareColumn(key);
  }
  for (const Aggregate& agg : flatten.aggregates_) {
    c->DeclareColumn(agg.output);
  }
  uint32_t groups = c->DeclareTemporaryColumn();
  c->DeclareSortedWithinGroups(flatten.keys_, {{flatten.out_ts_, false}});
  if (!flatten.keys_.empty()) {
    c->DeclareGroupNumbers(flatten.keys_, groups);
  }
  // Segments are output as each one ends, so batches can be small.
  c->DeclareSmallBatches();
}

const OperationRegistration& IntervalFlatten::registration() const {
  return kRegistration;
}

std::unique_ptr<PlanOperation> IntervalFlatten::Clone() const {
  return std::make_unique<IntervalFlatten>(*this);
}

void IntervalFlatten::Serialize(PlanCodec& c,
                                std::vector<PlanNodeId>*,
                                Available* available) {
  c.Column(*available, &ts_);
  c.Column(*available, &dur_);
  c.Columns(*available, &keys_);
  SerializeAggregates(c, *available, &aggregates_);
  c.NewColumn(&out_ts_, core::Int64{});
  c.NewColumn(&out_dur_, core::Int64{});
  *available = {out_ts_, out_dur_};
  available->insert(available->end(), keys_.begin(), keys_.end());
  for (const Aggregate& agg : aggregates_) {
    available->push_back(agg.output);
  }
}

}  // namespace perfetto::trace_processor::pipeline
