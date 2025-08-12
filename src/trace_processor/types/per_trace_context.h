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

#ifndef SRC_TRACE_PROCESSOR_TYPES_PER_TRACE_CONTEXT_H_
#define SRC_TRACE_PROCESSOR_TYPES_PER_TRACE_CONTEXT_H_

#include <memory>
#include "src/trace_processor/types/destructible.h"

namespace perfetto::trace_processor {

class ArgsTracker;
class ArgsTranslationTable;
class EventTracker;
class FlowTracker;
class GlobalArgsTracker;
class StackProfileTracker;
class ProcessTrackTranslationTable;
class SliceTracker;
class SliceTranslationTable;
class TraceFileTracker;
struct TraceProcessorContext;

// This struct holds all the state for a single trace parsing session.
// It is owned by the ForwardingTraceParser.
class PerTraceContext {
 public:
  PerTraceContext();
  ~PerTraceContext();

  PerTraceContext(PerTraceContext&&);
  PerTraceContext& operator=(PerTraceContext&&);

  void Init(TraceProcessorContext* context);

  // Keep the global tracker before the args tracker as we access the global
  // tracker in the destructor of the args tracker. Also keep it before other
  // trackers, as they may own ArgsTrackers themselves.
  std::shared_ptr<GlobalArgsTracker> global_args_tracker;
  std::unique_ptr<ArgsTracker> args_tracker;
  std::unique_ptr<ArgsTranslationTable> args_translation_table;
  std::unique_ptr<FlowTracker> flow_tracker;
  std::unique_ptr<EventTracker> event_tracker;
  std::unique_ptr<TraceFileTracker> trace_file_tracker;
  std::unique_ptr<StackProfileTracker> stack_profile_tracker;
  std::unique_ptr<ProcessTrackTranslationTable> process_track_translation_table;
  std::unique_ptr<SliceTracker> slice_tracker;
  std::unique_ptr<SliceTranslationTable> slice_translation_table;

  // These fields are stored as pointers to Destructible objects rather than
  // their actual type (a subclass of Destructible), as the concrete subclass
  // type is only available in storage_full target. To access these fields use
  // the GetOrCreate() method on their subclass type, e.g.
  // SyscallTracker::GetOrCreate(context)
  // clang-format off
  std::unique_ptr<Destructible> heap_graph_tracker; // HeapGraphTracker
  std::unique_ptr<Destructible> file_tracker;       // FileTracker
  std::unique_ptr<Destructible> etm_tracker;        // EtmTracker
  std::unique_ptr<Destructible> systrace_parser;    // SystraceParser
  // clang-format on

  std::unique_ptr<Destructible> content_analyzer;

  // Marks whether the uuid was read from the trace.
  // If the uuid was NOT read, the uuid will be made from the hash of the first
  // 4KB of the trace.
  bool uuid_found_in_trace = false;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_TYPES_PER_TRACE_CONTEXT_H_
