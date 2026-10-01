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
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/perfetto_sql/pipeline/catalog.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"

namespace perfetto::trace_processor::pipeline {

// The most columns a pipeline can output.
inline constexpr uint32_t kMaxPipelineColumns = 256;

// Writes an optimized plan as bytes, so that SQL can carry it and run it later
// without compiling the pipeline again.
std::string SerializePlan(const LogicalPlan&);

// Rebuilds a plan written by SerializePlan, looking its dataframes up in
// `catalog` again. Anyone can write bytes into SQL, so anything SerializePlan
// could not have written is refused, as is a plan whose dataframes have since
// changed.
base::StatusOr<LogicalPlan> DeserializePlan(std::string_view, const Catalog&);

// Points each scan of a dataframe argument at `args[i]`, the dataframe the
// plan is passed as its i-th argument when it runs, and types the scan's
// columns as the dataframe does. A null argument is a relation with no rows.
// Anyone can pass dataframes to a plan in SQL, so each must have the columns
// the plan reads from it.
base::Status BindDataframeArgs(
    LogicalPlan& plan,
    const std::vector<const dataframe::Dataframe*>& args,
    StringPool* pool);

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PLAN_SERIALIZATION_H_
