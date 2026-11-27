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

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "src/trace_processor/core/exec/interval_flatten.h"
#include "src/trace_processor/core/util/arena.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;

constexpr OperationRegistration IntervalFlatten::kRegistration{
    3, &MakeOperation<IntervalFlatten>};

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

InputNeeds IntervalFlatten::Needs(uint32_t) const {
  InputNeeds needs = IntervalLaneNeeds(ts_, dur_, keys_.span());
  AddAggregateNeeds(aggregates_, &needs);
  needs.full_batches = true;
  return needs;
}

OutputProperties IntervalFlatten::Derive(DeriveContext& c) const {
  // Its rows are the segments. Segments are output as each one ends, so batches
  // can be small. The operator always outputs group numbers.
  OutputProperties out;
  out.columns.push_back(c.ColumnOf(out_ts_));
  out.columns.push_back(c.ColumnOf(out_dur_));
  for (ColumnId key : keys_) {
    out.columns.push_back(c.ColumnOf(key));
  }
  for (const Aggregate& agg : aggregates_) {
    out.columns.push_back(c.ColumnOf(agg.output));
  }
  OutputProperties::Column groups = c.NewColumn();
  out.columns.push_back(groups);
  std::vector<ColumnId> keys(keys_.begin(), keys_.end());
  out.arrangements.push_back({keys, {{out_ts_, false}}});
  if (!keys.empty()) {
    out.group_numbers.push_back({std::move(keys), groups.id});
  }
  out.full_batches = false;
  return out;
}

void IntervalFlatten::Lower(Lowering* c, const PlanNode& node) const {
  c->LowerInput(node, 0);
  core::Arena& arena = c->arena();
  ex::IntervalFlattenSpec spec{c->QueryIntervalLanes(ts_, dur_, keys_.span()),
                               LowerAggregates(c, aggregates_)};
  c->AddOperator(arena.New<ex::IntervalFlatten>(arena, spec));
}

const OperationRegistration& IntervalFlatten::registration() const {
  return kRegistration;
}

void IntervalFlatten::Serialize(PlanCodec& c) {
  c.Pod(&ts_);
  c.Pod(&dur_);
  c.Vec(&keys_);
  SerializeAggregates(c, &aggregates_);
  c.Pod(&out_ts_);
  c.Pod(&out_dur_);
}

}  // namespace perfetto::trace_processor::pipeline
