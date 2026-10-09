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

#include "src/trace_processor/perfetto_sql/pipeline/aggregate.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/aggregate_function.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {

void PruneAggregates(std::vector<Aggregate>* aggregates,
                     std::vector<bool>* needed) {
  aggregates->erase(std::remove_if(aggregates->begin(), aggregates->end(),
                                   [&](const Aggregate& agg) {
                                     return !(*needed)[agg.output];
                                   }),
                    aggregates->end());
  for (const Aggregate& agg : *aggregates) {
    if (agg.reads_column()) {
      (*needed)[agg.column] = true;
    }
  }
}

core::exec::AggregateCall LowerAggregate(Lowering* c, const Aggregate& agg) {
  core::exec::AggregateCall call;
  call.function = agg.function;
  if (agg.reads_column()) {
    c->RequireInt64(agg.column);
    call.column = c->Position(agg.column);
  }
  return call;
}

void WriteAggregates(PlanWriter* c,
                     const std::vector<Aggregate>& aggregates,
                     const Available& available) {
  c->writer().Size(aggregates.size());
  for (const Aggregate& agg : aggregates) {
    c->writer().U8(static_cast<uint8_t>(agg.function));
    if (agg.reads_column()) {
      c->writer().Position(available, agg.column);
    }
    c->writer().Str(c->plan().columns()[agg.output].name);
  }
}

void ReadAggregates(PlanReader* c,
                    const Available& available,
                    std::vector<Aggregate>* aggregates) {
  aggregates->resize(c->reader().Count());
  for (Aggregate& agg : *aggregates) {
    uint8_t function = c->reader().U8();
    if (function > static_cast<uint8_t>(Aggregate::Function::kMax)) {
      c->reader().Fail();
      return;
    }
    agg.function = static_cast<Aggregate::Function>(function);
    if (agg.reads_column()) {
      agg.column = c->reader().Position(available);
    }
    agg.output = c->AddColumn(c->reader().Str(), core::Int64{});
  }
}

}  // namespace perfetto::trace_processor::pipeline
