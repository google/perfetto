/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use context.get() file except in compliance with the License.
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

#include "src/trace_processor/types/trace_processor_context.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/forwarding_trace_parser.h"
#include "src/trace_processor/importers/common/args_translation_table.h"
#include "src/trace_processor/importers/common/clock_converter.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/cpu_tracker.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/flow_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/mapping_tracker.h"
#include "src/trace_processor/importers/common/metadata_tracker.h"
#include "src/trace_processor/importers/common/process_track_translation_table.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/sched_event_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/stack_profile_tracker.h"
#include "src/trace_processor/importers/common/trace_file_tracker.h"
#include "src/trace_processor/importers/common/track_compressor.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/importers/proto/proto_trace_reader.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/trace_reader_registry.h"
#include "src/trace_processor/types/trace_processor_context_ptr.h"

namespace perfetto::trace_processor {
namespace {

template <typename T>
using Ptr = TraceProcessorContextPtr<T>;

void InitPerMachineState(TraceProcessorContext* context, uint32_t machine_id) {
  // Per-machine state.
  context->machine_tracker = Ptr<MachineTracker>::MakeRoot(context, machine_id);
  context->process_tracker = Ptr<ProcessTracker>::MakeRoot(context);
  context->clock_tracker = Ptr<ClockTracker>::MakeRoot(context);
  context->mapping_tracker = Ptr<MappingTracker>::MakeRoot(context);
  context->cpu_tracker = Ptr<CpuTracker>::MakeRoot(context);

  // Per-machine state (legacy).
  context->args_tracker = Ptr<ArgsTracker>::MakeRoot(context);
  context->track_tracker = Ptr<TrackTracker>::MakeRoot(context);
  context->track_compressor = Ptr<TrackCompressor>::MakeRoot(context);
  context->slice_tracker = Ptr<SliceTracker>::MakeRoot(context);
  context->slice_translation_table =
      Ptr<SliceTranslationTable>::MakeRoot(context->storage.get());
  context->flow_tracker = Ptr<FlowTracker>::MakeRoot(context);
  context->process_track_translation_table =
      Ptr<ProcessTrackTranslationTable>::MakeRoot(context->storage.get());
  context->event_tracker = Ptr<EventTracker>::MakeRoot(context);
  context->sched_event_tracker = Ptr<SchedEventTracker>::MakeRoot(context);
  context->stack_profile_tracker = Ptr<StackProfileTracker>::MakeRoot(context);
  context->args_translation_table =
      Ptr<ArgsTranslationTable>::MakeRoot(context->storage.get());

  context->slice_tracker->SetOnSliceBeginCallback(
      [context](TrackId track_id, SliceId slice_id) {
        context->flow_tracker->ClosePendingEventsOnTrack(track_id, slice_id);
      });
}

}  // namespace

TraceProcessorContext::TraceProcessorContext() = default;
TraceProcessorContext::TraceProcessorContext(const Config& _config) {
  // Global state.
  config = _config;
  storage = Ptr<TraceStorage>::MakeRoot(config);
  reader_registry = Ptr<TraceReaderRegistry>::MakeRoot();
  global_args_tracker = Ptr<GlobalArgsTracker>::MakeRoot(storage.get());
  trace_file_tracker = Ptr<TraceFileTracker>::MakeRoot(this);
  descriptor_pool_ = Ptr<DescriptorPool>::MakeRoot();

  // Root state.
  multi_machine_context = Ptr<MultiMachineContext>::MakeRoot();
  clock_converter = Ptr<ClockConverter>::MakeRoot(this);

  // Global state (per-trace).
  metadata_tracker = Ptr<MetadataTracker>::MakeRoot(storage.get());

  InitPerMachineState(this, 0);
}
TraceProcessorContext::~TraceProcessorContext() = default;

TraceProcessorContext* TraceProcessorContext::GetOrCreateContextForMachine(
    uint32_t raw_machine_id) const {
  PERFETTO_DCHECK(raw_machine_id != 0);

  auto [it, inserted] =
      multi_machine_context->machine_to_context.Insert(raw_machine_id, nullptr);

  // If we just inserted the element, the pointer will be null.
  if (inserted) {
    auto context = std::make_unique<TraceProcessorContext>();

    // Global state.
    context->config = config;
    context->storage = storage.Fork();
    context->sorter = sorter.Fork();
    context->reader_registry = reader_registry.Fork();
    context->global_args_tracker = global_args_tracker.Fork();
    context->trace_file_tracker = trace_file_tracker.Fork();
    context->descriptor_pool_ = descriptor_pool_.Fork();

    context->register_additional_proto_modules =
        register_additional_proto_modules;

    // Global state (per-trace).
    context->metadata_tracker = metadata_tracker.Fork();
    context->content_analyzer = content_analyzer.Fork();
    context->heap_graph_tracker = heap_graph_tracker.Fork();
    context->uuid_found_in_trace = uuid_found_in_trace;

    InitPerMachineState(context.get(), raw_machine_id);

    // TODO(lalitm): figure out where the right place to put this.
    context->process_tracker->SetPidZeroIsUpidZeroIdleProcess();

    *it = std::move(context);
  }
  return it->get();
}

std::optional<MachineId> TraceProcessorContext::machine_id() const {
  if (!machine_tracker) {
    // Doesn't require that |machine_tracker| is initialized, e.g. in unit
    // tests.
    return std::nullopt;
  }
  return machine_tracker->machine_id();
}

void TraceProcessorContext::DestroyNonEssential() {
  auto _storage = std::move(storage);

  // TODO(b/309623584): Decouple from storage and remove from here. This
  // function should only move storage and delete everything else.
  auto _heap_graph_tracker = std::move(heap_graph_tracker);
  auto _clock_converter = std::move(clock_converter);
  // "to_ftrace" textual converter of the "raw" table requires remembering the
  // kernel version (inside system_info_tracker) to know how to textualise
  // sched_switch.prev_state bitflags.
  auto _system_info_tracker = std::move(system_info_tracker);

  // "__intrinsic_winscope_proto_to_args_with_defaults" and trace summarization
  // both require the descriptor pool to be alive.
  auto _descriptor_pool_ = std::move(descriptor_pool_);

  this->~TraceProcessorContext();
  new (this) TraceProcessorContext();

  storage = std::move(_storage);
  heap_graph_tracker = std::move(_heap_graph_tracker);
  clock_converter = std::move(_clock_converter);
  system_info_tracker = std::move(_system_info_tracker);
  descriptor_pool_ = std::move(_descriptor_pool_);
}

}  // namespace perfetto::trace_processor
