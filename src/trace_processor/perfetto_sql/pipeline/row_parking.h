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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_ROW_PARKING_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_ROW_PARKING_H_

#include <cstdint>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/pipeline.h"

namespace perfetto::trace_processor::pipeline {

// The last pass over a pipeline's steps, after lowering has chosen them: it
// decides where rows are kept, so that no step ever holds rows.
//
// Lowering plans every reorder (StepColumns::Keeps::kRowNumbers) as if it
// kept every column. This pass makes it run on parked rows instead:
//  - just before it, ParkRows parks every column read after it, in a store of
//    its own, and adds the rows' numbers there, which the reorder orders;
//  - after it, a parked column is fetched back (FetchRows) just before the
//    first step which reads it, and not at all if none does.
// Each column is held once, by the store it was parked in, however many
// reorders follow. A column is dropped once no step reads it any more. The
// pass finds out which steps read what from StepColumns alone, and renumbers
// every step's columns to match.
//
// `steps` follow a source producing `source_columns` columns. `outputs` are
// the columns read after the pipeline, renumbered to where the pipeline now
// outputs them. If null, any column may be read after it, and each is put
// back where it was. Stores are numbered from `*next_store` on, which is
// advanced past them.
base::Status ParkReorderedRows(uint32_t source_columns,
                               std::vector<uint32_t>* outputs,
                               uint32_t* next_store,
                               std::vector<core::exec::Pipeline::Step>* steps);

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_ROW_PARKING_H_
