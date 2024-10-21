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
#include "protos/perfetto/trace/ftrace/hyp.pbzero.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"

namespace perfetto::trace_processor {
namespace {

TrackTracker::LegacyCharArrayName GetTrackName(uint32_t cpu) {
  return TrackTracker::LegacyCharArrayName{
      base::StackString<255>("pkVM Hypervisor CPU %u", cpu)};
}

}  // namespace

PkvmHypervisorCpuTracker::PkvmHypervisorCpuTracker(
    TraceProcessorContext* context)
    : context_(context),
      category_(context->storage->InternString("pkvm_hyp")),
      slice_name_(context->storage->InternString("hyp")),
      hyp_enter_reason_(context->storage->InternString("hyp_enter_reason")) {}

// static
bool PkvmHypervisorCpuTracker::IsPkvmHypervisorEvent(uint32_t event_id) {
  using protos::pbzero::FtraceEvent;
  switch (event_id) {
    case FtraceEvent::kHypEnterFieldNumber:
    case FtraceEvent::kHypExitFieldNumber:
    case FtraceEvent::kHostHcallFieldNumber:
    case FtraceEvent::kHostMemAbortFieldNumber:
    case FtraceEvent::kHostSmcFieldNumber:
      return true;
    default:
      return false;
  }
}

void PkvmHypervisorCpuTracker::ParseHypEvent(uint32_t cpu,
                                             int64_t timestamp,
                                             uint32_t event_id,
                                             protozero::ConstBytes blob) {
  using protos::pbzero::FtraceEvent;
  switch (event_id) {
    case FtraceEvent::kHypEnterFieldNumber:
      ParseHypEnter(cpu, timestamp);
      break;
    case FtraceEvent::kHypExitFieldNumber:
      ParseHypExit(cpu, timestamp);
      break;
    case FtraceEvent::kHostHcallFieldNumber:
      ParseHostHcall(cpu, blob);
      break;
    case FtraceEvent::kHostMemAbortFieldNumber:
      ParseHostMemAbort(cpu, blob);
      break;
    case FtraceEvent::kHostSmcFieldNumber:
      ParseHostSmc(cpu, blob);
      break;
    // TODO(b/249050813): add remaining hypervisor events
    default:
      PERFETTO_FATAL("Not a hypervisor event %d", event_id);
  }
}

void PkvmHypervisorCpuTracker::ParseHypEnter(uint32_t cpu, int64_t timestamp) {
  // TODO(b/249050813): handle bad events (e.g. 2 hyp_enter in a row)
  TrackId track_id = context_->track_tracker->InternCpuTrack(
      tracks::pkvm_hypervisor, cpu, GetTrackName(cpu));
  context_->slice_tracker->Begin(timestamp, track_id, category_, slice_name_);
}

void PkvmHypervisorCpuTracker::ParseHypExit(uint32_t cpu, int64_t timestamp) {
  // TODO(b/249050813): handle bad events (e.g. 2 hyp_exit in a row)
  TrackId track_id = context_->track_tracker->InternCpuTrack(
      tracks::pkvm_hypervisor, cpu, GetTrackName(cpu));
  context_->slice_tracker->End(timestamp, track_id);
}

void PkvmHypervisorCpuTracker::ParseHostHcall(uint32_t cpu,
                                              protozero::ConstBytes blob) {
  protos::pbzero::HostHcallFtraceEvent::Decoder evt(blob.data, blob.size);
  TrackId track_id = context_->track_tracker->InternCpuTrack(
      tracks::pkvm_hypervisor, cpu, GetTrackName(cpu));

  auto args_inserter = [this, &evt](ArgsTracker::BoundInserter* inserter) {
    StringId host_hcall = context_->storage->InternString("host_hcall");
    StringId id = context_->storage->InternString("id");
    StringId invalid = context_->storage->InternString("invalid");
    inserter->AddArg(hyp_enter_reason_, Variadic::String(host_hcall));
    inserter->AddArg(id, Variadic::UnsignedInteger(evt.id()));
    inserter->AddArg(invalid, Variadic::UnsignedInteger(evt.invalid()));
  };
  context_->slice_tracker->AddArgs(track_id, category_, slice_name_,
                                   args_inserter);
}

void PkvmHypervisorCpuTracker::ParseHostSmc(uint32_t cpu,
                                            protozero::ConstBytes blob) {
  protos::pbzero::HostSmcFtraceEvent::Decoder evt(blob.data, blob.size);
  TrackId track_id = context_->track_tracker->InternCpuTrack(
      tracks::pkvm_hypervisor, cpu, GetTrackName(cpu));

  auto args_inserter = [this, &evt](ArgsTracker::BoundInserter* inserter) {
    StringId host_smc = context_->storage->InternString("host_smc");
    StringId id = context_->storage->InternString("id");
    StringId forwarded = context_->storage->InternString("forwarded");
    inserter->AddArg(hyp_enter_reason_, Variadic::String(host_smc));
    inserter->AddArg(id, Variadic::UnsignedInteger(evt.id()));
    inserter->AddArg(forwarded, Variadic::UnsignedInteger(evt.forwarded()));
  };
  context_->slice_tracker->AddArgs(track_id, category_, slice_name_,
                                   args_inserter);
}

void PkvmHypervisorCpuTracker::ParseHostMemAbort(uint32_t cpu,
                                                 protozero::ConstBytes blob) {
  protos::pbzero::HostMemAbortFtraceEvent::Decoder evt(blob.data, blob.size);
  TrackId track_id = context_->track_tracker->InternCpuTrack(
      tracks::pkvm_hypervisor, cpu, GetTrackName(cpu));

  auto args_inserter = [this, &evt](ArgsTracker::BoundInserter* inserter) {
    StringId host_mem_abort = context_->storage->InternString("host_mem_abort");
    StringId esr = context_->storage->InternString("esr");
    StringId addr = context_->storage->InternString("addr");
    inserter->AddArg(hyp_enter_reason_, Variadic::String(host_mem_abort));
    inserter->AddArg(esr, Variadic::UnsignedInteger(evt.esr()));
    inserter->AddArg(addr, Variadic::UnsignedInteger(evt.addr()));
  };
  context_->slice_tracker->AddArgs(track_id, category_, slice_name_,
                                   args_inserter);
}

}  // namespace perfetto::trace_processor
