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

#ifndef SRC_TRACE_PROCESSOR_CORE_DATAFRAME_OPERATOR_LOWERING_H_
#define SRC_TRACE_PROCESSOR_CORE_DATAFRAME_OPERATOR_LOWERING_H_

#include "src/trace_processor/core/dataframe/logical_plan.h"
#include "src/trace_processor/core/dataframe/operator_plan.h"

namespace perfetto::trace_processor::core::dataframe {

// Lowers a logical plan for the operator executor.
//
// Only the meaning of each operation is read; the access-path strategy the
// planner recorded alongside it is ignored, since this backend brings its own
// kernels.
class OperatorLowering {
 public:
  static OperatorPlan Lower(const LogicalPlan& plan);
};

}  // namespace perfetto::trace_processor::core::dataframe

#endif  // SRC_TRACE_PROCESSOR_CORE_DATAFRAME_OPERATOR_LOWERING_H_
