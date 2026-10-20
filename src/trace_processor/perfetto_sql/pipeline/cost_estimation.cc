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

#include "src/trace_processor/perfetto_sql/pipeline/cost_estimation.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "src/trace_processor/core/common/duplicate_types.h"
#include "src/trace_processor/core/common/row_estimate.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"

namespace perfetto::trace_processor::pipeline {

PlanEstimator::PlanEstimator(const LogicalPlan& plan) : plan_(plan) {}

core::RowEstimate PlanEstimator::EstimateNode(PlanNodeId id) {
  const PlanNode& node = plan_.nodes()[id];
  return node.operation().Estimate(this, node);
}

uint32_t PlanEstimator::Rows(double rows) {
  return static_cast<uint32_t>(std::min(
      rows, static_cast<double>(std::numeric_limits<uint32_t>::max())));
}

void PlanEstimator::AddSortCost(uint32_t rows) {
  if (rows >= 2) {
    cost_ += kSortCostPerComparison * rows * std::log2(rows);
  }
}

void PlanEstimator::AddSearchCost(uint32_t rows) {
  cost_ += kSearchCostPerStep * std::log2(static_cast<double>(rows) + 1);
}

core::RowEstimate PlanEstimator::ApplyBlind(
    core::RowEstimate rows,
    const std::vector<WherePredicate>& predicates) {
  core::RowModel model(rows.estimated);
  for (const WherePredicate& predicate : predicates) {
    cost_ += kFilterCostPerRow * model.rows().estimated;
    model.ApplyFilter(predicate.op, core::HasDuplicates{}, 0,
                      /*non_null=*/false);
  }
  return {rows.max, model.rows().estimated};
}

PlanEstimate EstimatePlan(const LogicalPlan& plan) {
  PlanEstimator estimator(plan);
  PlanEstimate estimate;
  estimate.rows = estimator.EstimateNode(plan.root());
  estimate.cost = estimator.cost();
  return estimate;
}

}  // namespace perfetto::trace_processor::pipeline
