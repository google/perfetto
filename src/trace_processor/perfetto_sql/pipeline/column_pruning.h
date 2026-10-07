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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_COLUMN_PRUNING_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_COLUMN_PRUNING_H_

#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"

namespace perfetto::trace_processor::pipeline {

// Drops columns that nothing in the plan ends up using, so sources read less
// data and operators skip work whose results nobody reads. For SQL sources,
// the query itself is narrowed so SQLite can skip computing them too.
//
// The rows a plan produces are unchanged, though their order may differ.
void PruneColumns(LogicalPlan& plan);

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_COLUMN_PRUNING_H_
