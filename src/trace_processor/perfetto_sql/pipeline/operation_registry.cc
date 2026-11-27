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

#include "src/trace_processor/perfetto_sql/pipeline/operation_registry.h"

#include <cstdint>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/aggregate_stage.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/compute.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/distinct.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/graph_tree.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/interval_fill_gaps.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/interval_flatten.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/interval_intersect.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/join.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/limit.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/order_by.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/scan.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/tree_accumulate.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/tree_key_paths.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/where.h"

namespace perfetto::trace_processor::pipeline {
namespace {

constexpr const OperationRegistration* kOperations[] = {
    &Scan::kRegistration,
    &TreeAccumulate::kRegistration,
    &TreeKeyPaths::kRegistration,
    &IntervalIntersect::kRegistration,
    &IntervalFlatten::kRegistration,
    &IntervalFillGaps::kRegistration,
    &OrderBy::kRegistration,
    &AggregateStage::kRegistration,
    &Distinct::kRegistration,
    &Compute::kRegistration,
    &Where::kRegistration,
    &GraphTree::kRegistration,
    &Limit::kRegistration,
    &Join::kRegistration,
};

}  // namespace

PlanOperation* OperationRegistration::Make(core::Arena* arena) const {
  return make_(arena);
}

const OperationRegistration* FindOperationByTag(uint8_t tag) {
  const OperationRegistration* found = nullptr;
  for (const auto* registration : kOperations) {
    if (registration->tag() == tag) {
      // Operations sharing a tag would read each other's payloads.
      PERFETTO_CHECK(!found);
      found = registration;
    }
  }
  return found;
}

base::Status RunBoundPasses(BoundPlan* bound, const LoweringContext& context) {
  bound->planned.resize(bound->plan.nodes().size());
  for (PlanNodeId id = 0; id < bound->planned.size(); ++id) {
    bound->plan.nodes()[id].operation().Rewrite(bound, id, context);
    PlanNodeInputs(bound, id, context);
    RETURN_IF_ERROR(bound->status);
  }
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::pipeline
