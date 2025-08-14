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

#ifndef SRC_TRACE_PROCESSOR_TYPES_TRACE_PROCESSOR_CONTEXT_H_
#define SRC_TRACE_PROCESSOR_TYPES_TRACE_PROCESSOR_CONTEXT_H_

#include <cstdint>
#include <memory>
#include <optional>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/types/destructible.h"
#include "src/trace_processor/types/trace_processor_context_ptr.h"

namespace perfetto::trace_processor {

class ArgsTracker;
class ArgsTranslationTable;
class ClockConverter;
class ClockTracker;
class CpuTracker;
class DescriptorPool;
class EventTracker;
class FlowTracker;
class GlobalArgsTracker;
class MachineTracker;
class MappingTracker;
class MetadataTracker;
class ProcessTracker;
class ProcessTrackTranslationTable;
class ProtoTraceReader;
class RegisteredFileTracker;
class SchedEventTracker;
class SliceTracker;
class SliceTranslationTable;
class StackProfileTracker;
class SymbolTracker;
class TraceFileTracker;
class TraceReaderRegistry;
class TraceSorter;
class TraceStorage;
class TrackCompressor;
class TrackTracker;
struct ProtoImporterModuleContext;

using MachineId = tables::MachineTable::Id;

class TraceProcessorContext {
 public:
  template <typename T>
  using GlobalPtr = TraceProcessorContextPtr<T>;

  template <typename T>
  using RootPtr = TraceProcessorContextPtr<T>;

  template <typename T>
  using PerMachinePtr = TraceProcessorContextPtr<T>;

  class MultiMachineContext;

  // The default constructor is used in testing.
  TraceProcessorContext();
  ~TraceProcessorContext();

  TraceProcessorContext(const TraceProcessorContext&) = delete;
  TraceProcessorContext& operator=(const TraceProcessorContext&) = delete;

  static TraceProcessorContext CreateRootContext(const Config& config) {
    return TraceProcessorContext(config);
  }
  TraceProcessorContext* GetOrCreateContextForMachine(
      uint32_t raw_machine_id) const;
  void DestroyNonEssential();

  // Global State
  // ============
  //
  // This state is shared between all machines in a trace.
  // It is initialized once when the root TraceProcessorContext is created and
  // then shared between all machines.

  Config config;
  GlobalPtr<TraceStorage> storage;
  GlobalPtr<TraceSorter> sorter;
  GlobalPtr<TraceReaderRegistry> reader_registry;
  GlobalPtr<GlobalArgsTracker> global_args_tracker;
  GlobalPtr<TraceFileTracker> trace_file_tracker;
  GlobalPtr<DescriptorPool> descriptor_pool_;

  // The registration function for additional proto modules.
  // This is populated by TraceProcessorImpl to allow for late registration of
  // modules.
  using RegisterAdditionalProtoModulesFn = void(ProtoImporterModuleContext*,
                                                TraceProcessorContext*);
  RegisterAdditionalProtoModulesFn* register_additional_proto_modules = nullptr;

  // Root state
  // ============
  //
  // Only exists on the root TraceProcessorContext.
  RootPtr<MultiMachineContext> multi_machine_context;
  RootPtr<ClockConverter> clock_converter;

  // Per-Trace State (Miscategorized)
  // ==========================
  //
  // This state is shared between all machines in a trace but is specific to a
  // single trace.
  //
  // TODO(lalitm): this is miscategorized due to legacy reasons. It needs to be
  // moved to a "per-trace" category.

  GlobalPtr<MetadataTracker> metadata_tracker;
  GlobalPtr<RegisteredFileTracker> registered_file_tracker;
  GlobalPtr<Destructible> content_analyzer;
  GlobalPtr<Destructible> heap_graph_tracker;  // HeapGraphTracker

  // Marks whether the uuid was read from the trace.
  // If the uuid was NOT read, the uuid will be made from the hash of the first
  // 4KB of the trace.
  bool uuid_found_in_trace = false;

  // Per-Machine State
  // =================
  //
  // This state is unique to each machine in a trace.
  // It is initialized when a new machine is discovered in the trace.

  PerMachinePtr<SymbolTracker> symbol_tracker;
  PerMachinePtr<ProcessTracker> process_tracker;
  PerMachinePtr<ClockTracker> clock_tracker;
  PerMachinePtr<MappingTracker> mapping_tracker;
  PerMachinePtr<MachineTracker> machine_tracker;
  PerMachinePtr<CpuTracker> cpu_tracker;

  // Per-Machine, Per-Trace State
  // ==========================
  //
  // This state is unique to each (machine, trace) pair.
  //
  // TODO(lalitm): this is miscategorized due to legacy reasons. It needs to be
  // moved to a "per-trace per-trace" category.

  PerMachinePtr<ArgsTranslationTable> args_translation_table;
  PerMachinePtr<ProcessTrackTranslationTable> process_track_translation_table;
  PerMachinePtr<SliceTranslationTable> slice_translation_table;
  PerMachinePtr<ArgsTracker> args_tracker;
  PerMachinePtr<TrackTracker> track_tracker;
  PerMachinePtr<TrackCompressor> track_compressor;
  PerMachinePtr<SliceTracker> slice_tracker;
  PerMachinePtr<FlowTracker> flow_tracker;
  PerMachinePtr<EventTracker> event_tracker;
  PerMachinePtr<SchedEventTracker> sched_event_tracker;
  PerMachinePtr<StackProfileTracker> stack_profile_tracker;

  // These fields are stored as pointers to Destructible objects rather than
  // their actual type (a subclass of Destructible), as the concrete subclass
  // type is only available in storage_full target. To access these fields use
  // the GetOrCreate() method on their subclass type, e.g.
  // SyscallTracker::GetOrCreate(context)
  PerMachinePtr<Destructible> binder_tracker;        // BinderTracker
  PerMachinePtr<Destructible> syscall_tracker;       // SyscallTracker
  PerMachinePtr<Destructible> system_info_tracker;   // SystemInfoTracker
  PerMachinePtr<Destructible> systrace_parser;       // SystraceParser
  PerMachinePtr<Destructible> thread_state_tracker;  // ThreadStateTracker
  PerMachinePtr<Destructible> ftrace_sched_tracker;  // FtraceSchedEventTracker

  std::optional<MachineId> machine_id() const;

 private:
  explicit TraceProcessorContext(const Config& config);

  TraceProcessorContext(TraceProcessorContext&&) = default;
  TraceProcessorContext& operator=(TraceProcessorContext&&) = default;
};

class TraceProcessorContext::MultiMachineContext {
 public:
  base::FlatHashMap<uint32_t, std::unique_ptr<TraceProcessorContext>>
      machine_to_context;
  base::FlatHashMap<uint32_t, std::unique_ptr<ProtoTraceReader>> proto_readers;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_TYPES_TRACE_PROCESSOR_CONTEXT_H_
