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

#ifndef SRC_TRACE_PROCESSOR_TRACE_PROCESSOR_CONTEXT_H_
#define SRC_TRACE_PROCESSOR_TRACE_PROCESSOR_CONTEXT_H_

#include <memory>
#include <vector>

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/chunked_trace_reader.h"
#include "src/trace_processor/destructible.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"

namespace perfetto {
namespace trace_processor {

class ArgsTracker;
class ChunkedTraceReader;
class ClockTracker;
class EventTracker;
class FtraceModule;
class GlobalArgsTracker;
class HeapGraphTracker;
class HeapProfileTracker;
class MetadataTracker;
class ProcessTracker;
class SliceTracker;
class TraceParser;
class TraceSorter;
class TraceStorage;
class TrackTracker;

class TraceProcessorContext {
 public:
  TraceProcessorContext();
  ~TraceProcessorContext();

  Config config;

  std::unique_ptr<TraceStorage> storage;
  std::unique_ptr<TrackTracker> track_tracker;
  std::unique_ptr<SliceTracker> slice_tracker;
  std::unique_ptr<ProcessTracker> process_tracker;
  std::unique_ptr<EventTracker> event_tracker;
  std::unique_ptr<ClockTracker> clock_tracker;
  std::unique_ptr<TraceParser> parser;
  std::unique_ptr<TraceSorter> sorter;
  std::unique_ptr<ChunkedTraceReader> chunk_reader;
  std::unique_ptr<HeapProfileTracker> heap_profile_tracker;
  std::unique_ptr<MetadataTracker> metadata_tracker;

  // Keep the global tracker before the args tracker as we access the global
  // tracker in the destructor of the args tracker.
  std::unique_ptr<GlobalArgsTracker> global_args_tracker;
  std::unique_ptr<ArgsTracker> args_tracker;

  // These fields are stored as pointers to Destructible objects rather than
  // their actual type (a subclass of Destructible), as the concrete subclass
  // type is only available in the storage_full target. To access these fields,
  // use the GetOrCreate() method on their subclass type,
  // e.g. SyscallTracker::GetOrCreate(context).
  std::unique_ptr<Destructible> syscall_tracker;     // SyscallTracker
  std::unique_ptr<Destructible> sched_tracker;       // SchedEventTracker
  std::unique_ptr<Destructible> systrace_parser;     // SystraceParser
  std::unique_ptr<Destructible> heap_graph_tracker;  // HeapGraphTracker

  // This will be nullptr in the minimal build (storage_minimal target), and
  // a pointer to the instance of SystraceTraceParser class in the full build
  // (storage_full target). The corresponding initialization happens in
  // register_additional_modules.cc.
  std::unique_ptr<ChunkedTraceReader> systrace_trace_parser;

  // The module at the index N is registered to handle field id N in
  // TracePacket.
  std::vector<ProtoImporterModule*> modules_by_field;
  std::vector<std::unique_ptr<ProtoImporterModule>> modules;
  FtraceModule* ftrace_module = nullptr;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TRACE_PROCESSOR_CONTEXT_H_
