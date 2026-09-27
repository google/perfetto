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
#include <string_view>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/core/common/schema.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"

namespace perfetto::trace_processor::pipeline {

// The table function which runs a serialized plan.
inline constexpr char kPipelineFunction[] = "__intrinsic_pipeline";

// The aggregate which collects a relation's rows for a pipeline to read.
inline constexpr char kCollectFunction[] = "__intrinsic_rows";

// The most columns the table function can output.
inline constexpr uint32_t kMaxPipelineColumns = 256;

// The most relations a pipeline can read from SQL.
inline constexpr uint32_t kMaxPipelineInputs = 16;

// A plan whose SQL sources have been moved out into inputs: the plan reads its
// i-th SQL source as input i, and `inputs[i]` is the SQL collecting it.
struct PlanWithInputs {
  LogicalPlan plan;
  std::vector<std::string> inputs;
};
PlanWithInputs MoveSqlSourcesToInputs(LogicalPlan plan);

// SQL reading `plan`'s output under its own column names. The plan is carried
// in the SQL, serialized, and each relation it reads from SQL is collected
// where the SQL ends up, so it reads whatever is in scope there: a CTE, or the
// arguments of the function it is written in.
base::StatusOr<std::string> SelectPipelineSql(const LogicalPlan& plan);

// The columns a collection writes: the first argument of kCollectFunction.
std::string WriteCollectedColumns(const core::Schema& columns);
base::StatusOr<core::Schema> ReadCollectedColumns(std::string_view);

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PIPELINE_SQL_H_
