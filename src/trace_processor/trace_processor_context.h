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

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"

namespace perfetto {
namespace trace_processor {

class AndroidProbesModule;
class ArgsTracker;
class BinderTracker;
class ChunkedTraceReader;
class ClockTracker;
class EventTracker;
class FtraceModule;
class GraphicsEventModule;
class HeapGraphModule;
class HeapGraphTracker;
class HeapProfileTracker;
class ProcessTracker;
class SchedEventTracker;
class SliceTracker;
class SyscallTracker;
class SystemProbesModule;
class SystraceParser;
class TraceParser;
class TraceSorter;
class TraceStorage;
class TrackEventModule;
class TrackTracker;
class VulkanMemoryTracker;

class TraceProcessorContext {
 public:
  TraceProcessorContext();
  ~TraceProcessorContext();

  Config config;

  std::unique_ptr<TraceStorage> storage;
  std::unique_ptr<TrackTracker> track_tracker;
  std::unique_ptr<ArgsTracker> args_tracker;
  std::unique_ptr<SliceTracker> slice_tracker;
  std::unique_ptr<ProcessTracker> process_tracker;
  std::unique_ptr<SyscallTracker> syscall_tracker;
  std::unique_ptr<EventTracker> event_tracker;
  std::unique_ptr<SchedEventTracker> sched_tracker;
  std::unique_ptr<ClockTracker> clock_tracker;
  std::unique_ptr<TraceParser> parser;
  std::unique_ptr<TraceSorter> sorter;
  std::unique_ptr<ChunkedTraceReader> chunk_reader;
  std::unique_ptr<HeapProfileTracker> heap_profile_tracker;
  std::unique_ptr<SystraceParser> systrace_parser;
  std::unique_ptr<HeapGraphTracker> heap_graph_tracker;
  std::unique_ptr<VulkanMemoryTracker> vulkan_memory_tracker;
  std::unique_ptr<BinderTracker> binder_tracker;

  std::unique_ptr<ProtoImporterModule<FtraceModule>> ftrace_module;
  std::unique_ptr<ProtoImporterModule<TrackEventModule>> track_event_module;
  std::unique_ptr<ProtoImporterModule<SystemProbesModule>> system_probes_module;
  std::unique_ptr<ProtoImporterModule<AndroidProbesModule>>
      android_probes_module;
  std::unique_ptr<ProtoImporterModule<HeapGraphModule>> heap_graph_module;
  std::unique_ptr<ProtoImporterModule<GraphicsEventModule>>
      graphics_event_module;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TRACE_PROCESSOR_CONTEXT_H_
