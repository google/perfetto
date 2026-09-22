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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_PIPELINE_DATAFRAME_BUILDER_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_PIPELINE_DATAFRAME_BUILDER_H_

#include <string>
#include <string_view>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"

namespace perfetto::trace_processor {

// Runs `plan` and collects its result columns into a dataframe whose columns
// are `column_names`, one for each of the plan's.
//
// The dataframe is the one BuildRuntimeDataframeFromSqliteStatement would
// build from the same rows: a column takes the type of its first value unless
// `options` declares one, and a row which does not fit fails the build with
// the same message. Strings must already be interned in `pool`.
base::StatusOr<dataframe::Dataframe> BuildDataframeFromPipeline(
    StringPool* pool,
    std::vector<std::string> column_names,
    const pipeline::PhysicalPlan& plan,
    std::string_view error_context,
    const dataframe::AdhocDataframeBuilder::Options& options = {});

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_PIPELINE_DATAFRAME_BUILDER_H_
