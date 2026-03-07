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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_FUNCTIONS_HEAP_GRAPH_FUNCTIONS_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_FUNCTIONS_HEAP_GRAPH_FUNCTIONS_H_

#include "perfetto/base/status.h"

namespace perfetto::trace_processor {

class PerfettoSqlEngine;
class TraceProcessorContext;

// Registers heap graph helper functions:
//
// __INTRINSIC_HEAP_GRAPH_GET_ARRAY(array_data_id INT) -> BLOB
//   Returns the raw bytes of a primitive array stored during HPROF import.
//   The blob is in native little-endian format; element type and count are
//   available via heap_graph_object.array_element_type and
//   heap_graph_object.array_element_count.
//   Returns NULL if array_data_id is NULL or out of range.
base::Status RegisterHeapGraphFunctions(PerfettoSqlEngine* engine,
                                        TraceProcessorContext* context);

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_FUNCTIONS_HEAP_GRAPH_FUNCTIONS_H_
