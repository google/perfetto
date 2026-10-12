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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_AGGREGATE_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_AGGREGATE_H_

#include <vector>

#include "src/trace_processor/core/exec/aggregate_function.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"

namespace perfetto::trace_processor::pipeline {

class Lowering;
class PlanReader;
class PlanWriter;

// One aggregate a stage computes, e.g. `SUM(dur) AS total`: one of the
// executor's shared functions, the column it reads and the column it adds.
// Every stage which aggregates lists these, so a function added to the
// executor needs no stage of its own changed.
struct Aggregate {
  using Function = core::exec::AggregateCall::Function;
  Function function = Function::kCountStar;
  // Unused by COUNT(*).
  ColumnId column = 0;
  ColumnId output = 0;

  bool reads_column() const { return function != Function::kCountStar; }
};

// Drops the aggregates whose outputs aren't `needed`, and marks the columns
// the rest read.
void PruneAggregates(std::vector<Aggregate>* aggregates,
                     std::vector<bool>* needed);

// The executor's call for `aggregate`, its column at its lowered position.
core::exec::AggregateCall LowerAggregate(Lowering* c, const Aggregate&);

// Writes the aggregates, and reads them back adding their output columns.
void WriteAggregates(PlanWriter* c,
                     const std::vector<Aggregate>& aggregates,
                     const Available& available);
void ReadAggregates(PlanReader* c,
                    const Available& available,
                    std::vector<Aggregate>* aggregates);

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_AGGREGATE_H_
