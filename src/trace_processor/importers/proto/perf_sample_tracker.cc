/*
 * Copyright (C) 2021 The Android Open Source Project
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
#include "src/trace_processor/importers/proto/perf_sample_tracker.h"

#include <inttypes.h>
#include <stdio.h>

#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

#include "protos/perfetto/common/perf_events.pbzero.h"
#include "protos/perfetto/trace/profiling/profile_packet.pbzero.h"
#include "protos/perfetto/trace/trace_packet_defaults.pbzero.h"

namespace perfetto {
namespace trace_processor {

namespace {
// Follow perf tool naming convention.
const char* StringifyCounter(int32_t counter) {
  using protos::pbzero::PerfEvents;
  switch (counter) {
    case (PerfEvents::SW_CPU_CLOCK):
      return "cpu-clock";
    case (PerfEvents::SW_PAGE_FAULTS):
      return "page-faults";
    case (PerfEvents::HW_CPU_CYCLES):
      return "cpu-cycles";
    case (PerfEvents::HW_INSTRUCTIONS):
      return "instructions";
    default:
      break;
  }
  return "unknown";
}

StringId InternTimebaseCounterName(
    protos::pbzero::TracePacketDefaults::Decoder* defaults,
    TraceProcessorContext* context) {
  using namespace protos::pbzero;
  PerfSampleDefaults::Decoder perf_defaults(defaults->perf_sample_defaults());
  PerfEvents::Timebase::Decoder timebase(perf_defaults.timebase());

  if (timebase.counter() != PerfEvents::UNKNOWN_COUNTER) {
    return context->storage->InternString(StringifyCounter(timebase.counter()));
  }
  PerfEvents::Tracepoint::Decoder tracepoint(timebase.tracepoint());
  return context->storage->InternString(tracepoint.name());
}
}  // namespace

PerfSampleTracker::SamplingStreamInfo PerfSampleTracker::GetSamplingStreamInfo(
    uint32_t seq_id,
    uint32_t cpu,
    protos::pbzero::TracePacketDefaults::Decoder* nullable_defaults) {
  auto seq_it = seq_state_.find(seq_id);
  if (seq_it == seq_state_.end()) {
    seq_it = seq_state_.emplace(seq_id, next_perf_session_id_++).first;
  }
  SequenceState* seq_state = &seq_it->second;
  uint32_t session_id = seq_state->perf_session_id;

  auto cpu_it = seq_state->per_cpu.find(cpu);
  if (cpu_it != seq_state->per_cpu.end())
    return {seq_state->perf_session_id, cpu_it->second.timebase_track_id};

  // No defaults means legacy producer implementation, assume default timebase
  // of per-cpu timer. Always the case for Android R builds, and it isn't worth
  // guaranteeing support for intermediate S builds in this aspect.
  StringId name_id = kNullStringId;
  if (!nullable_defaults || !nullable_defaults->has_perf_sample_defaults()) {
    name_id = context_->storage->InternString(
        StringifyCounter(protos::pbzero::PerfEvents::SW_CPU_CLOCK));
  } else {
    name_id = InternTimebaseCounterName(nullable_defaults, context_);
  }

  TrackId timebase_track_id = context_->track_tracker->CreatePerfCounterTrack(
      name_id, session_id, cpu, /*is_timebase=*/true);

  seq_state->per_cpu.emplace(cpu, timebase_track_id);

  return {session_id, timebase_track_id};
}

}  // namespace trace_processor
}  // namespace perfetto
