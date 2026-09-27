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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PLAN_SERIALIZATION_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PLAN_SERIALIZATION_H_

#include <cstdint>
#include <string>
#include <string_view>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/perfetto_sql/pipeline/catalog.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"

namespace perfetto::trace_processor::pipeline {

// The table function which runs a plan, and the most columns it can output.
inline constexpr char kPipelineFunction[] = "__intrinsic_pipeline";
inline constexpr uint32_t kMaxPipelineColumns = 256;

// SQL reading `plan`'s output under its own column names, with the plan
// serialized into it.
base::StatusOr<std::string> SelectPipeline(const LogicalPlan& plan);

// Writes an optimized plan as bytes, so that SQL can carry it and run it later
// without compiling the pipeline again.
std::string SerializePlan(const LogicalPlan&);

// Rebuilds a plan from SerializePlan's bytes, looking its dataframes up in
// `catalog` again. Anyone can write bytes into SQL, so anything SerializePlan
// could not have written is refused, as is a plan whose dataframes have
// changed since.
base::StatusOr<LogicalPlan> DeserializePlan(std::string_view, const Catalog&);

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PLAN_SERIALIZATION_H_
