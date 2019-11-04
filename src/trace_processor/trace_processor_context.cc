/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/trace_processor/trace_processor_context.h"

#include "src/trace_processor/args_tracker.h"
#include "src/trace_processor/binder_tracker.h"
#include "src/trace_processor/chunked_trace_reader.h"
#include "src/trace_processor/clock_tracker.h"
#include "src/trace_processor/event_tracker.h"
#include "src/trace_processor/heap_profile_tracker.h"
#include "src/trace_processor/importers/ftrace/ftrace_module.h"
#include "src/trace_processor/importers/ftrace/sched_event_tracker.h"
#include "src/trace_processor/importers/json/json_trace_parser.h"
#include "src/trace_processor/importers/proto/android_probes_module.h"
#include "src/trace_processor/importers/proto/graphics_event_module.h"
#include "src/trace_processor/importers/proto/heap_graph_module.h"
#include "src/trace_processor/importers/proto/heap_graph_tracker.h"
#include "src/trace_processor/importers/proto/proto_trace_parser.h"
#include "src/trace_processor/importers/proto/system_probes_module.h"
#include "src/trace_processor/importers/proto/track_event_module.h"
#include "src/trace_processor/importers/systrace/systrace_parser.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/slice_tracker.h"
#include "src/trace_processor/stack_profile_tracker.h"
#include "src/trace_processor/syscall_tracker.h"
#include "src/trace_processor/trace_sorter.h"
#include "src/trace_processor/track_tracker.h"
#include "src/trace_processor/vulkan_memory_tracker.h"

namespace perfetto {
namespace trace_processor {

TraceProcessorContext::TraceProcessorContext() = default;
TraceProcessorContext::~TraceProcessorContext() = default;

}  // namespace trace_processor
}  // namespace perfetto
