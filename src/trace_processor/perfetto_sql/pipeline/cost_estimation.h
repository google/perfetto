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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_COST_ESTIMATION_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_COST_ESTIMATION_H_

#include <cstdint>
#include <vector>

#include "src/trace_processor/core/common/row_estimate.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"

namespace perfetto::trace_processor::pipeline {

// -----------------------------------------------------------------------------
// Cost estimation API
// -----------------------------------------------------------------------------

// What running a plan is expected to take: what SQLite needs to plan the
// query a pipeline is part of, as it plans the tables around it.
struct PlanEstimate {
  // The rows the plan produces.
  core::RowEstimate rows;
  // In the units a dataframe query's estimated cost is in, so that SQLite
  // can weigh a pipeline against the tables it is joined with.
  double cost = 0;
};

// Estimates `plan` without running any of it, so a plan need not be loaded.
// A WHERE straight on a dataframe is estimated as it is lowered: searches of
// sorted columns or an index, then tests of the rows they find, the rows each
// predicate keeps estimated as the dataframe planner estimates them from the
// dataframe's columns. A dataframe argument
// is only built once the query runs, so its size is assumed, and assumed
// large: overestimating a pipeline costs a worse join order, while
// underestimating one can have SQLite run it once per row of another table.
PlanEstimate EstimatePlan(const LogicalPlan& plan);

// -----------------------------------------------------------------------------
// Estimation
// -----------------------------------------------------------------------------

// Interface used by operation implementations to estimate a plan. Accumulates
// the cost of the work operations do; each operation estimates its own
// children through EstimateNode and returns the rows it produces.
class PlanEstimator {
 public:
  // Rows assumed for a relation whose size is unknown until the query runs.
  static constexpr uint32_t kAssumedRows = 100000;

  // Rough relative costs, per row, of the work operators do, in the scale the
  // dataframe's own operations are costed in (see the kCost of each
  // bytecode): reading and testing a value is a few units, sorting a few more
  // per comparison.
  static constexpr double kReadCostPerRow = 1;
  static constexpr double kFilterCostPerRow = 5;
  static constexpr double kHashCostPerRow = 5;
  static constexpr double kSearchCostPerStep = 5;
  static constexpr double kSortCostPerComparison = 10;
  static constexpr double kFoldCostPerRow = 10;
  static constexpr double kSweepCostPerRow = 10;

  explicit PlanEstimator(const LogicalPlan& plan);

  // Dispatches to the operation, which is responsible for estimating its
  // inputs.
  core::RowEstimate EstimateNode(PlanNodeId);

  // Adds the cost of sorting `rows`.
  void AddSortCost(uint32_t rows);
  // Adds the cost of a binary search of `rows`.
  void AddSearchCost(uint32_t rows);
  void AddCost(double cost) { cost_ += cost; }

  // Applies `predicates` to `rows` knowing nothing of the columns' values, as
  // the dataframe planner does for a column it has no statistics on.
  core::RowEstimate ApplyBlind(core::RowEstimate rows,
                               const std::vector<WherePredicate>& predicates);

  // A row count, saturating rather than wrapping.
  static uint32_t Rows(double rows);

  const LogicalPlan& plan() const { return plan_; }
  double cost() const { return cost_; }

 private:
  // Borrowed for the duration of estimation.
  const LogicalPlan& plan_;
  double cost_ = 0;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_COST_ESTIMATION_H_
