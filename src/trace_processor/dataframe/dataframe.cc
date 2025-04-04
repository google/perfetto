/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/dataframe/dataframe.h"

#include <cstdint>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/dataframe/impl/query_plan.h"
#include "src/trace_processor/dataframe/specs.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto::trace_processor::dataframe {

base::StatusOr<Dataframe::QueryPlan> Dataframe::PlanQuery(
    std::vector<FilterSpec>& specs,
    uint64_t cols_used) {
  if (specs.size() >= impl::kMaxFilters) {
    return base::ErrStatus(
        "Too many filters provided on a single dataframe. We only support up "
        "to 16 filters for performance reasons.");
  }
  ASSIGN_OR_RETURN(auto plan, impl::QueryPlanBuilder::Build(
                                  row_count_, columns_, specs, cols_used));
  return QueryPlan(std::move(plan));
}

}  // namespace perfetto::trace_processor::dataframe
