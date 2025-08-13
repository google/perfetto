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

namespace perfetto::trace_processor {
namespace {

void InitPerMachineState(TraceProcessorContext* context, uint32_t machine_id) {
  // Per-machine state.
  context->machine_tracker =
      std::make_unique<MachineTracker>(context, machine_id);
  context->process_tracker = std::make_unique<ProcessTracker>(context);
  context->clock_tracker = std::make_unique<ClockTracker>(context);
  context->mapping_tracker = std::make_unique<MappingTracker>(context);
  context->cpu_tracker = std::make_unique<CpuTracker>(context);

  // Per-machine state (legacy).
  context->args_tracker = std::make_unique<ArgsTracker>(context);
  context->track_tracker = std::make_unique<TrackTracker>(context);
  context->track_compressor = std::make_unique<TrackCompressor>(context);
  context->slice_tracker = std::make_unique<SliceTracker>(context);
  context->slice_translation_table =
      std::make_unique<SliceTranslationTable>(context->storage.get());
  context->flow_tracker = std::make_unique<FlowTracker>(context);
  context->process_track_translation_table =
      std::make_unique<ProcessTrackTranslationTable>(context->storage.get());
  context->event_tracker = std::make_unique<EventTracker>(context);
  context->sched_event_tracker = std::make_unique<SchedEventTracker>(context);
  context->stack_profile_tracker =
      std::make_unique<StackProfileTracker>(context);
  context->args_translation_table =
      std::make_unique<ArgsTranslationTable>(context->storage.get());

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
  storage = std::make_shared<TraceStorage>(config);
  reader_registry = std::make_shared<TraceReaderRegistry>();
  global_args_tracker = std::make_shared<GlobalArgsTracker>(storage.get());
  trace_file_tracker = std::make_shared<TraceFileTracker>(this);
  descriptor_pool_ = std::make_shared<DescriptorPool>();

  // Root state.
  multi_machine_context = std::make_unique<MultiMachineContext>();
  clock_converter = std::make_unique<ClockConverter>(this);

  // Global state (per-trace).
  metadata_tracker = std::make_unique<MetadataTracker>(storage.get());

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
    context->storage = storage;
    context->sorter = sorter;
    context->reader_registry = reader_registry;
    context->global_args_tracker = global_args_tracker;
    context->trace_file_tracker = trace_file_tracker;
    context->descriptor_pool_ = descriptor_pool_;

    context->register_additional_proto_modules =
        register_additional_proto_modules;

    // Global state (per-trace).
    context->metadata_tracker = metadata_tracker;
    context->content_analyzer = content_analyzer;
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
  TraceProcessorContext ctx;

  ctx.storage = std::move(storage);

  // TODO(b/309623584): Decouple from storage and remove from here. This
  // function should only move storage and delete everything else.
  ctx.heap_graph_tracker = std::move(heap_graph_tracker);
  ctx.clock_converter = std::move(clock_converter);
  // "to_ftrace" textual converter of the "raw" table requires remembering the
  // kernel version (inside system_info_tracker) to know how to textualise
  // sched_switch.prev_state bitflags.
  ctx.system_info_tracker = std::move(system_info_tracker);

  // "__intrinsic_winscope_proto_to_args_with_defaults" and trace summarization
  // both require the descriptor pool to be alive.
  ctx.descriptor_pool_ = std::move(descriptor_pool_);

  *this = std::move(ctx);
}

}  // namespace perfetto::trace_processor
