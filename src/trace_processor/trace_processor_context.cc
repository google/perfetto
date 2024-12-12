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

#include "src/trace_processor/types/trace_processor_context.h"

#include <memory>
#include <optional>

#include "perfetto/base/logging.h"
#include "src/trace_processor/forwarding_trace_parser.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/args_translation_table.h"
#include "src/trace_processor/importers/common/async_track_set_tracker.h"
#include "src/trace_processor/importers/common/clock_converter.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/cpu_tracker.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/flow_tracker.h"
#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/importers/common/legacy_v8_cpu_profile_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/mapping_tracker.h"
#include "src/trace_processor/importers/common/metadata_tracker.h"
#include "src/trace_processor/importers/common/process_track_translation_table.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/sched_event_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/slice_translation_table.h"
#include "src/trace_processor/importers/common/stack_profile_tracker.h"
#include "src/trace_processor/importers/common/trace_file_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/proto/android_track_event.descriptor.h"
#include "src/trace_processor/importers/proto/chrome_track_event.descriptor.h"
#include "src/trace_processor/importers/proto/multi_machine_trace_manager.h"
#include "src/trace_processor/importers/proto/perf_sample_tracker.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/importers/proto/track_event.descriptor.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/trace_reader_registry.h"

namespace perfetto::trace_processor {

TraceProcessorContext::TraceProcessorContext(const InitArgs& args)
    : config(args.config), storage(args.storage) {
  reader_registry = std::make_unique<TraceReaderRegistry>(this);
  // Init the trackers.
  machine_tracker.reset(new MachineTracker(this, args.raw_machine_id));
  if (!machine_id()) {
    multi_machine_trace_manager.reset(new MultiMachineTraceManager(this));
  }
  track_tracker.reset(new TrackTracker(this));
  async_track_set_tracker.reset(new AsyncTrackSetTracker(this));
  args_tracker.reset(new ArgsTracker(this));
  args_translation_table.reset(new ArgsTranslationTable(storage.get()));
  slice_tracker.reset(new SliceTracker(this));
  slice_translation_table.reset(new SliceTranslationTable(storage.get()));
  flow_tracker.reset(new FlowTracker(this));
  event_tracker.reset(new EventTracker(this));
  sched_event_tracker.reset(new SchedEventTracker(this));
  process_tracker.reset(new ProcessTracker(this));
  process_track_translation_table.reset(
      new ProcessTrackTranslationTable(storage.get()));
  clock_tracker.reset(new ClockTracker(this));
  clock_converter.reset(new ClockConverter(this));
  mapping_tracker.reset(new MappingTracker(this));
  perf_sample_tracker.reset(new PerfSampleTracker(this));
  stack_profile_tracker.reset(new StackProfileTracker(this));
  metadata_tracker.reset(new MetadataTracker(storage.get()));
  cpu_tracker.reset(new CpuTracker(this));
  global_args_tracker.reset(new GlobalArgsTracker(storage.get()));
  {
    descriptor_pool_.reset(new DescriptorPool());
    auto status = descriptor_pool_->AddFromFileDescriptorSet(
        kTrackEventDescriptor.data(), kTrackEventDescriptor.size());

    PERFETTO_DCHECK(status.ok());

    status = descriptor_pool_->AddFromFileDescriptorSet(
        kChromeTrackEventDescriptor.data(), kChromeTrackEventDescriptor.size());

    PERFETTO_DCHECK(status.ok());

    status = descriptor_pool_->AddFromFileDescriptorSet(
        kAndroidTrackEventDescriptor.data(),
        kAndroidTrackEventDescriptor.size());

    PERFETTO_DCHECK(status.ok());
  }

  slice_tracker->SetOnSliceBeginCallback(
      [this](TrackId track_id, SliceId slice_id) {
        flow_tracker->ClosePendingEventsOnTrack(track_id, slice_id);
      });

  trace_file_tracker = std::make_unique<TraceFileTracker>(this);
  legacy_v8_cpu_profile_tracker =
      std::make_unique<LegacyV8CpuProfileTracker>(this);
}

TraceProcessorContext::TraceProcessorContext() = default;
TraceProcessorContext::~TraceProcessorContext() = default;

TraceProcessorContext::TraceProcessorContext(TraceProcessorContext&&) = default;
TraceProcessorContext& TraceProcessorContext::operator=(
    TraceProcessorContext&&) = default;

std::optional<MachineId> TraceProcessorContext::machine_id() const {
  if (!machine_tracker) {
    // Doesn't require that |machine_tracker| is initialzed, e.g. in unit tests.
    return std::nullopt;
  }
  return machine_tracker->machine_id();
}

}  // namespace perfetto::trace_processor
