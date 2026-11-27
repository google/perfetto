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

#include "src/trace_processor/perfetto_sql/pipeline/operations/interval_intersect.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/core/exec/interval_intersect.h"
#include "src/trace_processor/core/exec/interval_lanes.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/util/arena.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;

constexpr OperationRegistration IntervalIntersect::kRegistration{
    2, &MakeOperation<IntervalIntersect>};

std::optional<uint32_t> IntervalIntersect::Prune(std::vector<bool>* needed) {
  // An intersection passes on only the operand columns used after it, but
  // reads each operand's bounds and partition columns to find the regions.
  auto& isect = *this;
  for (IntervalIntersect::Operand& operand : isect.operands_) {
    operand.carried.RemoveIf([&](ColumnId id) { return !(*needed)[id]; });
  }
  for (const IntervalIntersect::Operand& operand : isect.operands_) {
    (*needed)[operand.ts] = true;
    (*needed)[operand.dur] = true;
    for (ColumnId key : operand.keys) {
      (*needed)[key] = true;
    }
  }
  return std::nullopt;
}

InputNeeds IntervalIntersect::Needs(uint32_t input) const {
  const Operand& operand = operands_[input];
  return IntervalLaneNeeds(operand.ts, operand.dur, operand.keys.span());
}

std::vector<std::vector<InputColumn>> IntervalIntersect::CommonTypes() const {
  // Keys are compared across operands, so each key must have the same type in
  // all of them.
  std::vector<std::vector<InputColumn>> common;
  uint32_t key_count = operands_.empty() ? 0 : operands_[0].keys.size();
  for (uint32_t k = 0; k < key_count; ++k) {
    std::vector<InputColumn> same;
    for (uint32_t i = 0; i < operands_.size(); ++i) {
      same.push_back({i, operands_[i].keys[k]});
    }
    common.push_back(std::move(same));
  }
  return common;
}

OutputProperties IntervalIntersect::Derive(DeriveContext& c) const {
  // The region's bounds come first, then each operand's carried columns in
  // turn.
  OutputProperties out;
  out.columns.push_back(c.ColumnOf(ts_));
  out.columns.push_back(c.ColumnOf(dur_));
  for (const Operand& operand : operands_) {
    for (ColumnId id : operand.carried) {
      out.columns.push_back(c.ColumnOf(id));
    }
  }
  // The output is grouped by the keys, if every key is carried to the output by
  // some operand.
  uint32_t key_count = operands_.empty() ? 0 : operands_[0].keys.size();
  std::vector<ColumnId> carried_keys;
  for (uint32_t k = 0; k < key_count; ++k) {
    for (const Operand& operand : operands_) {
      const auto& carried = operand.carried;
      if (std::find(carried.begin(), carried.end(), operand.keys[k]) !=
          carried.end()) {
        carried_keys.push_back(operand.keys[k]);
        break;
      }
    }
  }
  if (key_count > 0 && carried_keys.size() == key_count) {
    out.arrangements.push_back({carried_keys, {}});
  }
  return out;
}

void IntervalIntersect::Lower(Lowering* c, const PlanNode& node) const {
  PERFETTO_DCHECK(!c->has_source());
  PERFETTO_DCHECK(operands_.size() == node.children().size());
  // Each operand is read from its own side pipeline.
  core::Arena& arena = c->arena();
  auto inputs = arena.NewSpan<ex::CollectedInputSpec>(
      static_cast<uint32_t>(operands_.size()));
  for (uint32_t i = 0; i < operands_.size(); i++) {
    const Operand& operand = operands_[i];
    Lowering& side = c->AddSidePipeline();
    side.LowerInput(node, i);
    ex::CollectedInputSpec lowered;
    lowered.lanes =
        side.QueryIntervalLanes(operand.ts, operand.dur, operand.keys);
    lowered.retained_columns = side.QueryPositions(operand.carried);
    lowered.pipeline = side.FinishSide();
    inputs[i] = lowered;
  }
  c->SetSource(arena.New<ex::IntervalIntersect>(inputs));
}

const OperationRegistration& IntervalIntersect::registration() const {
  return kRegistration;
}

void IntervalIntersect::Serialize(PlanCodec& c) {
  c.Pod(&ts_);
  c.Pod(&dur_);
  c.Resize(&operands_);
  PERFETTO_NO_VECTORIZE_LOOP
  for (Operand& operand : operands_) {
    c.Pod(&operand.ts);
    c.Pod(&operand.dur);
    c.Vec(&operand.keys);
    c.Vec(&operand.carried);
  }
}

}  // namespace perfetto::trace_processor::pipeline
