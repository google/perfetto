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
    // TODO(lalitm): Replace this with numeric normalization and support
    // Double totals. With dynamically typed input, a Double in a later batch
    // may require promoting earlier Int64 values. Picking one numeric type
    // for the whole column may then need a breaker that buffers the input.
    c->RequireType(agg.column, core::Int64{});
    call.column = c->QueryPosition(agg.column);
  }
  return call;
}

void SerializeAggregates(PlanCodec& c,
                         const Available& available,
                         std::vector<Aggregate>* aggregates) {
  aggregates->resize(c.Count(aggregates->size()));
  for (Aggregate& agg : *aggregates) {
    c.Enum(&agg.function, Aggregate::Function::kMax);
    if (agg.reads_column()) {
      c.Column(available, &agg.column);
    }
    c.NewColumn(&agg.output, core::Int64{});
  }
}

}  // namespace perfetto::trace_processor::pipeline
