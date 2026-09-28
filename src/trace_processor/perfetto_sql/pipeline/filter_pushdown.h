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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_FILTER_PUSHDOWN_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_FILTER_PUSHDOWN_H_

#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"

namespace perfetto::trace_processor::pipeline {

// Moves each filter's conditions as far down the plan as they go without
// changing its rows, so fewer rows reach the operators above and, at a scan,
// the source runs them itself. A condition reaches:
// - a scan below it, which then reads only the rows it keeps;
// - an intersection operand's scan, if it tests a column that operand
//   carries: a region carries the columns of one row of each operand, so
//   dropping the row drops exactly the regions it would be tested in. A PER
//   column holds the same value in every operand, so a condition on one
//   reaches all of them.
// It never passes a TREE ACCUMULATE: dropping a node changes the totals of
// the nodes around it. A filter left with no conditions is removed.
void PushDownFilters(LogicalPlan& plan);

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_FILTER_PUSHDOWN_H_
