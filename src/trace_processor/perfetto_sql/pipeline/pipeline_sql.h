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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PIPELINE_SQL_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PIPELINE_SQL_H_

#include <cstdint>
#include <string>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {

// -----------------------------------------------------------------------------
// SQL integration
// -----------------------------------------------------------------------------

// The table function which runs a serialized plan. It outputs at most
// kMaxPipelineColumns columns.
inline constexpr char kPipelineFunction[] = "__intrinsic_pipeline";

// The aggregate which builds a dataframe of a relation's rows, for a pipeline
// to read: `__intrinsic_dataframe_agg('a,b', a, b)`.
inline constexpr char kDataframeAggFunction[] = "__intrinsic_dataframe_agg";

// The function which gathers the dataframes a pipeline reads into the one
// argument the table function takes them in:
// `__intrinsic_dataframes((SELECT ...), (SELECT ...))`.
inline constexpr char kDataframesFunction[] = "__intrinsic_dataframes";

// A plan whose SQL sources have been moved out into dataframe arguments: the
// plan reads its i-th SQL source as dataframe argument i, and `args[i]` is the
// SQL building it.
struct PlanWithDataframeArgs {
  LogicalPlan plan;
  std::vector<std::string> args;
};
PlanWithDataframeArgs MoveSqlSourcesToDataframeArgs(LogicalPlan plan);

// SQL reading `plan`'s output under its own column names. The plan is
// serialized into the SQL, and each relation it reads from SQL is built into a
// dataframe where that SQL runs, so it sees whatever is in scope there.
base::StatusOr<std::string> SelectPipelineSql(const LogicalPlan& plan);

// -----------------------------------------------------------------------------
// SQL quoting
// -----------------------------------------------------------------------------

std::string QuoteIdentifier(const std::string& name);

std::string QuoteString(const std::string& text);

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PIPELINE_SQL_H_
