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

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"

namespace perfetto::trace_processor::pipeline {

// The table function which runs a serialized plan.
inline constexpr char kPipelineFunction[] = "__intrinsic_pipeline";

// The most columns the table function can output.
inline constexpr uint32_t kMaxPipelineColumns = 256;

// SQL reading `plan`'s output under its own column names. The plan is carried
// in the SQL, serialized, so the SQL runs it wherever it ends up.
base::StatusOr<std::string> SelectPipelineSql(const LogicalPlan& plan);

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PIPELINE_SQL_H_
