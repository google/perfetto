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

#include "src/trace_processor/core/common/row_estimate.h"
#include "src/trace_processor/perfetto_sql/pipeline/catalog.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"

namespace perfetto::trace_processor::pipeline {

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
// A scan of a dataframe, with its filters, is estimated as the dataframe
// estimates the same query, finding the dataframe in `catalog`. A dataframe
// argument is only built once the query runs, so its size is assumed, and
// assumed large: overestimating a pipeline costs a worse join order, while
// underestimating one can have SQLite run it once per row of another table.
PlanEstimate EstimatePlan(const LogicalPlan& plan, const Catalog& catalog);

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_COST_ESTIMATION_H_
