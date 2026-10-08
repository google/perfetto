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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_PROJECTION_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_PROJECTION_H_

#include "src/trace_processor/perfetto_sql/pipeline/operation_registry.h"

// The pipeline stages which only change which columns the row has and what
// they are called. They add no plan node, so they are just registrations.
namespace perfetto::trace_processor::pipeline::projection {

extern const OperationRegistration kSelect;
extern const OperationRegistration kExtend;
extern const OperationRegistration kDrop;
extern const OperationRegistration kRename;
extern const OperationRegistration kSet;
extern const OperationRegistration kAs;

}  // namespace perfetto::trace_processor::pipeline::projection

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_PROJECTION_H_
