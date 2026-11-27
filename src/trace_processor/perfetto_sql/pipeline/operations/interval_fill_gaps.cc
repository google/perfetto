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

#include "src/trace_processor/perfetto_sql/pipeline/operations/interval_fill_gaps.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/core/exec/interval_fill_gaps.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/util/arena.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;

constexpr OperationRegistration IntervalFillGaps::kRegistration{
    6, &MakeOperation<IntervalFillGaps>};

std::optional<uint32_t> IntervalFillGaps::Prune(std::vector<bool>* needed) {
  outputs_.RemoveIf(
      [&](const Output& output) { return !(*needed)[output.column]; });
  // ts, dur and the PER columns on both sides decide where the fillers go.
  for (ColumnId id : {ts_, dur_, background_ts_, background_dur_}) {
    (*needed)[id] = true;
  }
  for (ColumnId key : keys_) {
    (*needed)[key] = true;
  }
  for (ColumnId key : background_keys_) {
    (*needed)[key] = true;
  }
  for (const Output& output : outputs_) {
    (*needed)[output.input] = true;
    if (output.background) {
      (*needed)[*output.background] = true;
    }
  }
  return std::nullopt;
}

InputNeeds IntervalFillGaps::Needs(uint32_t input) const {
  return input == 0 ? IntervalLaneNeeds(ts_, dur_, keys_.span())
                    : IntervalLaneNeeds(background_ts_, background_dur_,
                                        background_keys_.span());
}

std::vector<std::vector<InputColumn>> IntervalFillGaps::CommonTypes() const {
  std::vector<std::vector<InputColumn>> common;
  // Columns present on both sides are converted to a common type.
  for (const Output& output : outputs_) {
    if (output.background) {
      common.push_back({{0, output.input}, {1, *output.background}});
    }
  }
  // PER columns are numbered in one table, so both sides need one type even
  // if they are not output.
  for (uint32_t k = 0; k < background_keys_.size(); ++k) {
    common.push_back({{0, keys_[k]}, {1, background_keys_[k]}});
  }
  return common;
}

OutputProperties IntervalFillGaps::Derive(DeriveContext& c) const {
  // The output has no known order. Filler batches can be small.
  OutputProperties out;
  for (const Output& output : outputs_) {
    out.columns.push_back(c.ColumnOf(output.column));
  }
  out.full_batches = false;
  return out;
}

void IntervalFillGaps::Lower(Lowering* c, const PlanNode& node) const {
  const auto& fill = *this;
  c->LowerInput(node, 0);
  // The background is read from its own side pipeline.
  Lowering& background = c->AddSidePipeline();
  background.LowerInput(node, 1);

  ex::IntervalFillGapsSpec spec;
  spec.input = c->QueryIntervalLanes(fill.ts_, fill.dur_, fill.keys_.span());
  spec.input.forever = true;
  spec.background.lanes = background.QueryIntervalLanes(
      fill.background_ts_, fill.background_dur_, fill.background_keys_.span());
  core::Arena& arena = c->arena();
  std::vector<uint32_t> retained;
  spec.outputs = arena.NewSpan<ex::IntervalFillGapsSpec::Output>(
      static_cast<uint32_t>(fill.outputs_.size()));
  for (uint32_t i = 0; i < fill.outputs_.size(); ++i) {
    const Output& output = fill.outputs_[i];
    ex::IntervalFillGapsSpec::Output lowered;
    lowered.input = c->QueryPosition(output.input);
    if (output.background) {
      // Only the background columns fillers take are kept.
      lowered.background = static_cast<uint32_t>(retained.size());
      retained.push_back(background.QueryPosition(*output.background));
    }
    const auto& column = c->plan().columns()[output.column];
    lowered.fallback =
        column.type.storage().value_or(core::StorageType{core::Int64{}});
    lowered.name = arena.CopyString(c->plan().names().text(column.name));
    spec.outputs[i] = lowered;
  }
  spec.background.retained_columns = arena.CopySpan<uint32_t>(retained);
  spec.background.pipeline = background.FinishSide();
  c->AddOperator(arena.New<ex::IntervalFillGaps>(arena, spec));
}

const OperationRegistration& IntervalFillGaps::registration() const {
  return kRegistration;
}

void IntervalFillGaps::Serialize(PlanCodec& c) {
  c.Pod(&ts_);
  c.Pod(&dur_);
  c.Vec(&keys_);
  c.Pod(&background_ts_);
  c.Pod(&background_dur_);
  c.Vec(&background_keys_);
  c.Resize(&outputs_);
  PERFETTO_NO_VECTORIZE_LOOP
  for (Output& output : outputs_) {
    c.Pod(&output.column);
    c.Pod(&output.input);
    bool from_background = output.background.has_value();
    c.Pod(&from_background);
    ColumnId background = output.background.value_or(0);
    c.Pod(&background);
    if (from_background) {
      output.background = background;
    }
  }
}

}  // namespace perfetto::trace_processor::pipeline
