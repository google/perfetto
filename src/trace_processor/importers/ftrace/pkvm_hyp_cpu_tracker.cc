/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/importers/ftrace/pkvm_hyp_cpu_tracker.h"

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"

namespace perfetto {
namespace trace_processor {

PkvmHypervisorCpuTracker::PkvmHypervisorCpuTracker(
    TraceProcessorContext* context)
    : context_(context),
      pkvm_hyp_id_(context->storage->InternString("pkvm_hyp")) {}

// static
bool PkvmHypervisorCpuTracker::IsPkvmHypervisorEvent(uint16_t event_id) {
  using protos::pbzero::FtraceEvent;
  switch (event_id) {
    case FtraceEvent::kHypEnterFieldNumber:
    case FtraceEvent::kHypExitFieldNumber:
      return true;
    default:
      return false;
  }
}

void PkvmHypervisorCpuTracker::ParseHypEvent(uint32_t cpu,
                                             int64_t timestamp,
                                             uint16_t event_id) {
  using protos::pbzero::FtraceEvent;
  switch (event_id) {
    case FtraceEvent::kHypEnterFieldNumber:
      ParseHypEnter(cpu, timestamp);
      break;
    case FtraceEvent::kHypExitFieldNumber:
      ParseHypExit(cpu, timestamp);
      break;
    // TODO(b/249050813): add remaining hypervisor events
    default:
      PERFETTO_FATAL("Not a hypervisor event %d", event_id);
  }
}

void PkvmHypervisorCpuTracker::ParseHypEnter(uint32_t cpu, int64_t timestamp) {
  // TODO(b/249050813): handle bad events (e.g. 2 hyp_enter in a row)

  // TODO(b/249050813): ideally we want to add here a reason for entering
  // hypervisor (e.g. host_hcall). However, such reason comes in a separate
  // hypevisor event, so for the time being use a very generic "in hyp" name.
  // TODO(b/249050813): figure out the UI story once we add hyp events.
  base::StackString<255> slice_name("in hyp");
  StringId slice_id = context_->storage->InternString(slice_name.string_view());

  StringId track_id = GetHypCpuTrackId(cpu);
  TrackId track = context_->track_tracker->InternCpuTrack(track_id, cpu);
  context_->slice_tracker->Begin(timestamp, track, pkvm_hyp_id_, slice_id);
}

void PkvmHypervisorCpuTracker::ParseHypExit(uint32_t cpu, int64_t timestamp) {
  // TODO(b/249050813): handle bad events (e.g. 2 hyp_exit in a row)
  StringId track_id = GetHypCpuTrackId(cpu);
  TrackId track = context_->track_tracker->InternCpuTrack(track_id, cpu);
  context_->slice_tracker->End(timestamp, track);
}

StringId PkvmHypervisorCpuTracker::GetHypCpuTrackId(uint32_t cpu) {
  base::StackString<255> track_name("pkVM Hypervisor CPU %d", cpu);
  return context_->storage->InternString(track_name.string_view());
}

}  // namespace trace_processor
}  // namespace perfetto
