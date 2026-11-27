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

#include "src/trace_processor/perfetto_sql/pipeline/operations/join.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/join.h"
#include "src/trace_processor/core/util/arena.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {

namespace ex = core::exec;

constexpr OperationRegistration Join::kRegistration{13, &MakeOperation<Join>};

std::optional<uint32_t> Join::Prune(std::vector<bool>* needed) {
  outputs_.RemoveIf([&](ColumnId c) { return !(*needed)[c]; });
  (*needed)[probe_key_] = true;
  (*needed)[build_key_] = true;
  return std::nullopt;
}

InputNeeds Join::Needs(uint32_t input) const {
  // The executor reads the probe key in whatever integer type it's stored as.
  InputNeeds needs;
  needs.types_first.push_back(
      {input == 0 ? probe_key_ : build_key_, core::Int64{}, input == 0});
  return needs;
}

OutputProperties Join::Derive(DeriveContext& c) const {
  // A left join outputs a batch's unmatched rows after its matched ones, so
  // the output has no known order.
  OutputProperties out;
  out.columns = c.input(0).columns;
  for (ColumnId column : outputs_) {
    out.columns.push_back(c.ColumnOf(column));
  }
  out.full_batches = false;
  return out;
}

void Join::Lower(Lowering* c, const PlanNode& node) const {
  c->LowerInput(node, 0);
  // The right side runs as its own side pipeline, which ends in the join's
  // build.
  Lowering& build = c->AddSidePipeline();
  build.LowerInput(node, 1);
  uint32_t key = build.QueryPosition(build_key_);
  // The build side stores only the columns the join outputs.
  core::ArenaSpan<bool> unread = build.arena().NewSpan<bool>(
      static_cast<uint32_t>(build.QueryRows().columns.size()));
  std::fill(unread.begin(), unread.end(), true);
  core::ArenaSpan<ex::JoinSpec::Output> outputs =
      c->arena().NewSpan<ex::JoinSpec::Output>(
          static_cast<uint32_t>(outputs_.size()));
  for (uint32_t i = 0; i < outputs_.size(); ++i) {
    uint32_t position = build.QueryPosition(outputs_[i]);
    unread[position] = false;
    const PlanType& type = build.QueryRows().columns[position].type;
    outputs[i] = {position,
                  type.storage().value_or(core::StorageType{core::Int64{}})};
  }
  uint32_t build_pipeline =
      build.FinishSide(build.arena().New<ex::JoinBuild>(key, unread));
  ex::JoinSpec spec{
      left_ ? ex::JoinSpec::Kind::kLeft : ex::JoinSpec::Kind::kInner,
      c->QueryPosition(probe_key_), build_pipeline, outputs};
  c->AddOperator(c->arena().New<ex::Join>(spec));
}

void Join::Serialize(PlanCodec& c) {
  c.Pod(&left_);
  c.Pod(&probe_key_);
  c.Pod(&build_key_);
  c.Vec(&outputs_);
}

const OperationRegistration& Join::registration() const {
  return kRegistration;
}

}  // namespace perfetto::trace_processor::pipeline
