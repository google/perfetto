/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_redaction/remap_scheduling_events.h"

#include "src/trace_redaction/proto_util.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/ftrace/sched.pbzero.h"

namespace perfetto::trace_redaction {

namespace {
int32_t RemapPid(const Context& context,
                 uint64_t timestamp,
                 uint32_t cpu,
                 int32_t pid) {
  PERFETTO_DCHECK(context.package_uid.value());
  PERFETTO_DCHECK(cpu < context.synthetic_threads->tids.size());

  auto slice = context.timeline->Search(timestamp, pid);

  auto expected_uid = NormalizeUid(slice.uid);
  auto actual_uid = NormalizeUid(context.package_uid.value());

  return !pid || expected_uid == actual_uid
             ? pid
             : context.synthetic_threads->tids[cpu];
}
}  // namespace

base::Status ThreadMergeRemapFtraceEventPid::Redact(
    const Context& context,
    const protos::pbzero::FtraceEventBundle::Decoder& bundle,
    protozero::ProtoDecoder& event,
    protos::pbzero::FtraceEvent* event_message) const {
  if (!context.package_uid.has_value()) {
    return base::ErrStatus(
        "ThreadMergeRemapFtraceEventPid: missing package uid");
  }

  if (!context.synthetic_threads.has_value()) {
    return base::ErrStatus(
        "ThreadMergeRemapFtraceEventPid: missing synthetic threads");
  }

  // This should never happen. A bundle should have a cpu.
  if (!bundle.has_cpu()) {
    return base::ErrStatus(
        "ThreadMergeRemapFtraceEventPid: Invalid ftrace event, missing cpu.");
  }

  if (bundle.cpu() >= context.synthetic_threads->tids.size()) {
    return base::ErrStatus(
        "ThreadMergeRemapFtraceEventPid: synthetic thread count");
  }

  auto timestamp =
      event.FindField(protos::pbzero::FtraceEvent::kTimestampFieldNumber);

  // This should never happen. An event should have a timestamp.
  if (!timestamp.valid()) {
    return base::ErrStatus(
        "ThreadMergeRemapFtraceEventPid: Invalid ftrace event, missing "
        "timestamp.");
  }

  // This handler should only be called for the pid field.
  auto pid = event.FindField(protos::pbzero::FtraceEvent::kPidFieldNumber);
  PERFETTO_DCHECK(pid.valid());

  // The event's pid is technically a uint, but we need it as a int.
  auto new_pid =
      RemapPid(context, timestamp.as_uint64(), bundle.cpu(), pid.as_int32());
  event_message->set_pid(static_cast<uint32_t>(new_pid));

  return base::OkStatus();
}

base::Status ThreadMergeRemapSchedSwitchPid::Redact(
    const Context& context,
    const protos::pbzero::FtraceEventBundle::Decoder& bundle,
    protozero::ProtoDecoder& event,
    protos::pbzero::FtraceEvent* event_message) const {
  if (!context.package_uid.has_value()) {
    return base::ErrStatus(
        "ThreadMergeRemapSchedSwitchPid: missing package uid");
  }

  if (!context.synthetic_threads.has_value()) {
    return base::ErrStatus(
        "ThreadMergeRemapSchedSwitchPid: missing synthetic threads");
  }

  // This should never happen. A bundle should have a cpu.
  if (!bundle.has_cpu()) {
    return base::ErrStatus(
        "ThreadMergeRemapSchedSwitchPid: Invalid ftrace event, missing cpu.");
  }

  if (bundle.cpu() >= context.synthetic_threads->tids.size()) {
    return base::ErrStatus(
        "ThreadMergeRemapSchedSwitchPid: synthetic thread count");
  }

  auto timestamp =
      event.FindField(protos::pbzero::FtraceEvent::kTimestampFieldNumber);

  // This should never happen. An event should have a timestamp.
  if (!timestamp.valid()) {
    return base::ErrStatus(
        "ThreadMergeRemapSchedSwitchPid: Invalid ftrace event, missing "
        "timestamp.");
  }

  // This handler should only be called for the sched switch field.
  auto sched_switch =
      event.FindField(protos::pbzero::FtraceEvent::kSchedSwitchFieldNumber);
  PERFETTO_DCHECK(sched_switch.valid());

  protozero::ProtoDecoder sched_switch_decoder(sched_switch.as_bytes());

  auto old_prev_pid_field = sched_switch_decoder.FindField(
      protos::pbzero::SchedSwitchFtraceEvent::kPrevPidFieldNumber);
  auto old_next_pid_field = sched_switch_decoder.FindField(
      protos::pbzero::SchedSwitchFtraceEvent::kNextPidFieldNumber);

  if (!old_prev_pid_field.valid()) {
    return base::ErrStatus(
        "ThreadMergeRemapSchedSwitchPid: Invalid sched switch event, missing "
        "prev pid");
  }

  if (!old_next_pid_field.valid()) {
    return base::ErrStatus(
        "ThreadMergeRemapSchedSwitchPid: Invalid sched switch event, missing "
        "next pid");
  }

  auto new_prev_pid_field =
      RemapPid(context, timestamp.as_uint64(), bundle.cpu(),
               old_prev_pid_field.as_int32());
  auto new_next_pid_field =
      RemapPid(context, timestamp.as_uint64(), bundle.cpu(),
               old_next_pid_field.as_int32());

  auto* sched_switch_message = event_message->set_sched_switch();

  for (auto f = sched_switch_decoder.ReadField(); f.valid();
       f = sched_switch_decoder.ReadField()) {
    switch (f.id()) {
      case protos::pbzero::SchedSwitchFtraceEvent::kPrevPidFieldNumber:
        sched_switch_message->set_prev_pid(new_prev_pid_field);
        break;

      case protos::pbzero::SchedSwitchFtraceEvent::kNextPidFieldNumber:
        sched_switch_message->set_next_pid(new_next_pid_field);
        break;

      default:
        proto_util::AppendField(f, sched_switch_message);
        break;
    }
  }

  return base::OkStatus();
}

base::Status ThreadMergeRemapSchedWakingPid::Redact(
    const Context& context,
    const protos::pbzero::FtraceEventBundle::Decoder& bundle,
    protozero::ProtoDecoder& event,
    protos::pbzero::FtraceEvent* event_message) const {
  if (!context.package_uid.has_value()) {
    return base::ErrStatus(
        "ThreadMergeRemapSchedWakingPid: missing package uid");
  }

  if (!context.synthetic_threads.has_value()) {
    return base::ErrStatus(
        "ThreadMergeRemapSchedWakingPid: missing synthetic threads");
  }

  // This should never happen. A bundle should have a cpu.
  if (!bundle.has_cpu()) {
    return base::ErrStatus(
        "ThreadMergeRemapSchedWakingPid: Invalid ftrace event, missing cpu.");
  }

  if (bundle.cpu() >= context.synthetic_threads->tids.size()) {
    return base::ErrStatus(
        "ThreadMergeRemapSchedWakingPid: synthetic thread count");
  }

  auto timestamp =
      event.FindField(protos::pbzero::FtraceEvent::kTimestampFieldNumber);

  // This should never happen. An event should have a timestamp.
  if (!timestamp.valid()) {
    return base::ErrStatus(
        "ThreadMergeRemapSchedWakingPid: Invalid ftrace event, missing "
        "timestamp.");
  }

  // This handler should only be called for the sched waking field.
  auto sched_waking =
      event.FindField(protos::pbzero::FtraceEvent::kSchedWakingFieldNumber);
  PERFETTO_DCHECK(sched_waking.valid());

  protozero::ProtoDecoder sched_waking_decoder(sched_waking.as_bytes());

  auto old_pid = sched_waking_decoder.FindField(
      protos::pbzero::SchedWakingFtraceEvent::kPidFieldNumber);

  if (!old_pid.valid()) {
    return base::ErrStatus(
        "ThreadMergeRemapSchedWakingPid: Invalid sched waking event, missing "
        "pid");
  }

  auto new_pid_field = RemapPid(context, timestamp.as_uint64(), bundle.cpu(),
                                old_pid.as_int32());

  auto* sched_waking_message = event_message->set_sched_waking();

  for (auto f = sched_waking_decoder.ReadField(); f.valid();
       f = sched_waking_decoder.ReadField()) {
    if (f.id() == protos::pbzero::SchedWakingFtraceEvent::kPidFieldNumber) {
      sched_waking_message->set_pid(new_pid_field);
    } else {
      proto_util::AppendField(f, sched_waking_message);
    }
  }

  return base::OkStatus();
}

// By doing nothing, the field gets dropped.
base::Status ThreadMergeDropField::Redact(
    const Context&,
    const protos::pbzero::FtraceEventBundle::Decoder&,
    protozero::ProtoDecoder&,
    protos::pbzero::FtraceEvent*) const {
  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
