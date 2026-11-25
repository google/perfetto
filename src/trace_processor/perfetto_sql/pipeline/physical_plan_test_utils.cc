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

#include "src/trace_processor/perfetto_sql/pipeline/physical_plan_test_utils.h"

#include <cstdint>
#include <memory>
#include <string>

#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/pipeline_formatter.h"
#include "src/trace_processor/core/exec/sink.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"

namespace perfetto::trace_processor::pipeline {

std::string PhysicalPlanToString(const PhysicalPlan& plan) {
  using core::exec::PipelineFormatter;
  std::string out;
  // A breaker's pipeline is shown where the next reads it.
  const core::exec::Pipelines& pipelines = plan.pipelines();
  for (uint32_t i = 0; i < pipelines.size(); ++i) {
    const core::exec::Sink* sink = pipelines[i].sink;
    if (sink && sink->IsBreaker()) {
      continue;
    }
    const auto& side =
        static_cast<const core::exec::Pipeline&>(*pipelines[i].pipeline);
    out += "side: " + PipelineFormatter::Format(side, &pipelines) + "\n";
  }
  return out +
         "main: " + PipelineFormatter::Format(plan.pipeline(), &pipelines);
}

}  // namespace perfetto::trace_processor::pipeline
