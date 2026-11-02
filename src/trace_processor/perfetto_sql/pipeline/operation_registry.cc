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

#include "perfetto/base/logging.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/aggregate_stage.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/compute.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/distinct.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/graph_tree.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/interval_fill_gaps.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/interval_flatten.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/interval_intersect.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/limit.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/order_by.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/projection.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/scan.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/tree_accumulate.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/where.h"

namespace perfetto::trace_processor::pipeline {

namespace {

const OperationRegistration* const kOperations[] = {
    &Scan::kRegistration,
    &TreeAccumulate::kRegistration,
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
    &projection::kSelect,
    &projection::kExtend,
    &projection::kDrop,
    &projection::kRename,
    &projection::kSet,
    &projection::kAs,
};

}  // namespace

std::unique_ptr<PlanOperation> OperationRegistration::Encoding::Make() const {
  return make_();
}

const OperationRegistration* FindOperationByTag(uint8_t tag) {
  const OperationRegistration* found = nullptr;
  for (const auto* registration : kOperations) {
    if (registration->encoding() && registration->encoding()->tag() == tag) {
      // Operations sharing a tag would read each other's payloads.
      PERFETTO_CHECK(!found);
      found = registration;
    }
  }
  return found;
}

const OperationRegistration* FindOperationBySyntax(uint32_t syntax) {
  for (const auto* registration : kOperations) {
    if (registration->syntax() == syntax) {
      return registration;
    }
  }
  return nullptr;
}

}  // namespace perfetto::trace_processor::pipeline
