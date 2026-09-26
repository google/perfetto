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

// Removes from every source of `plan` the columns nothing reads: neither the
// plan's output nor any operator between the source and the output. A SQL
// source is rewritten to select only what is kept, so SQLite does not compute
// the rest either.
//
// Works from the last stage back to the sources: each operator is told which
// of its output columns are needed and adds the input columns it needs to
// produce them. Pruning never changes which rows a plan produces, only which
// columns they carry.
void PruneColumns(LogicalPlan& plan);

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_COLUMN_PRUNING_H_
