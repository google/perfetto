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
#include <optional>
#include <utility>
#include <vector>

#include "src/trace_processor/core/exec/hash_aggregate.h"
#include "src/trace_processor/core/util/arena.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;

constexpr OperationRegistration AggregateStage::kRegistration{
    5, &MakeOperation<AggregateStage>};

std::optional<uint32_t> AggregateStage::Prune(std::vector<bool>* needed) {
  PruneAggregates(&aggregates_, needed);
  // Keys define the groups, so keep them even if nothing reads them.
  for (ColumnId key : keys_) {
    (*needed)[key] = true;
  }
  return std::nullopt;
}

InputNeeds AggregateStage::Needs(uint32_t) const {
  InputNeeds needs;
  AddAggregateNeeds(aggregates_, &needs);
  needs.full_batches = true;
  return needs;
}

OutputProperties AggregateStage::Derive(DeriveContext& c) const {
  // The output has one row per group. Groups are output in the order first
  // seen, so if the input is sorted by the keys, the output is too.
  OutputProperties out;
  for (ColumnId key : keys_) {
    out.columns.push_back(c.ColumnOf(key));
  }
  for (const Aggregate& agg : aggregates_) {
    out.columns.push_back(c.ColumnOf(agg.output));
  }
  std::vector<SortKey> sorted = c.input(0).SortedBy(keys_.span());
  if (!sorted.empty()) {
    out.arrangements.push_back({{}, std::move(sorted)});
  }
  return out;
}

void AggregateStage::Lower(Lowering* c, const PlanNode& node) const {
  c->LowerInput(node, 0);
  core::Arena& arena = c->arena();
  c->AddOperator(arena.New<ex::HashAggregate>(
      arena, ex::HashAggregateSpec{c->QueryPositions(keys_.span()),
                                   LowerAggregates(c, aggregates_)}));
}

const OperationRegistration& AggregateStage::registration() const {
  return kRegistration;
}

// Replaces `available` with the output columns.
void AggregateStage::Serialize(PlanCodec& c) {
  c.Vec(&keys_);
  SerializeAggregates(c, &aggregates_);
}

}  // namespace perfetto::trace_processor::pipeline
