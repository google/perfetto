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

#include "src/trace_processor/perfetto_sql/pipeline/operations/aggregate_stage.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/interval_flatten.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/interval_intersect.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/order_by.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/projection.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/scan.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/tree_accumulate.h"

namespace perfetto::trace_processor::pipeline {

namespace {

const OperationRegistration* const kOperations[] = {
    &Scan::kRegistration,
    &TreeAccumulate::kRegistration,
    &IntervalIntersect::kRegistration,
    &IntervalFlatten::kRegistration,
    &OrderBy::kRegistration,
    &AggregateStage::kRegistration,
    &projection::kSelect,
    &projection::kExtend,
    &projection::kDrop,
    &projection::kRename,
    &projection::kSet,
    &projection::kAs,
};

}  // namespace

const OperationRegistration* FindOperationByTag(uint8_t tag) {
  for (const auto* registration : kOperations) {
    if (registration->encoding() && registration->encoding()->tag() == tag) {
      return registration;
    }
  }
  return nullptr;
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
