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

#include "src/trace_redaction/redact_sched_switch.h"

#include "src/trace_redaction/proto_util.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/ftrace/sched.pbzero.h"

namespace perfetto::trace_redaction {

namespace {

protozero::ConstChars SanitizeCommValue(const Context& context,
                                        ProcessThreadTimeline::Slice slice,
                                        protozero::Field field) {
  if (NormalizeUid(slice.uid) == NormalizeUid(context.package_uid.value())) {
    return field.as_string();
  }

  return {};
}

}  // namespace

// Redact sched switch trace events in an ftrace event bundle:
//
//  event {
//    timestamp: 6702093744772646
//    pid: 0
//    sched_switch {
//      prev_comm: "swapper/0"
//      prev_pid: 0
//      prev_prio: 120
//      prev_state: 0
//      next_comm: "writer"
//      next_pid: 23020
//      next_prio: 96
//    }
//  }
//
// In the above message, it should be noted that "event.pid" will always be
// equal to "event.sched_switch.prev_pid".
//
// "ftrace_event_bundle_message" is the ftrace event bundle (contains a
// collection of ftrace event messages) because data in a sched_switch message
// is needed in order to know if the event should be added to the bundle.

base::Status RedactSchedSwitch::Redact(
    const Context& context,
    const protos::pbzero::FtraceEventBundle::Decoder&,
    protozero::ProtoDecoder& event,
    protos::pbzero::FtraceEvent* event_message) const {
  if (!context.package_uid.has_value()) {
    return base::ErrStatus("RedactSchedSwitch: missing package uid");
  }

  if (!context.timeline) {
    return base::ErrStatus("RedactSchedSwitch: missing timeline");
  }

  // The timestamp is needed to do the timeline look-up. If the packet has no
  // timestamp, don't add the sched switch event. This is the safest option.
  auto timestamp =
      event.FindField(protos::pbzero::FtraceEvent::kTimestampFieldNumber);
  if (!timestamp.valid()) {
    return base::OkStatus();
  }

  auto sched_switch =
      event.FindField(protos::pbzero::FtraceEvent::kSchedSwitchFieldNumber);
  if (!sched_switch.valid()) {
    return base::ErrStatus(
        "RedactSchedSwitch: was used for unsupported field type");
  }

  protozero::ProtoDecoder sched_switch_decoder(sched_switch.as_bytes());

  auto prev_pid = sched_switch_decoder.FindField(
      protos::pbzero::SchedSwitchFtraceEvent::kPrevPidFieldNumber);
  auto next_pid = sched_switch_decoder.FindField(
      protos::pbzero::SchedSwitchFtraceEvent::kNextPidFieldNumber);

  // There must be a prev pid and a next pid. Otherwise, the event is invalid.
  // Dropping the event is the safest option.
  if (!prev_pid.valid() || !next_pid.valid()) {
    return base::OkStatus();
  }

  // Avoid making the message until we know that we have prev and next pids.
  auto sched_switch_message = event_message->set_sched_switch();

  auto prev_slice =
      context.timeline->Search(timestamp.as_uint64(), prev_pid.as_int32());
  auto next_slice =
      context.timeline->Search(timestamp.as_uint64(), next_pid.as_int32());

  for (auto field = sched_switch_decoder.ReadField(); field.valid();
       field = sched_switch_decoder.ReadField()) {
    switch (field.id()) {
      case protos::pbzero::SchedSwitchFtraceEvent::kNextCommFieldNumber:
        sched_switch_message->set_next_comm(
            SanitizeCommValue(context, next_slice, field));
        break;

      case protos::pbzero::SchedSwitchFtraceEvent::kPrevCommFieldNumber:
        sched_switch_message->set_prev_comm(
            SanitizeCommValue(context, prev_slice, field));
        break;

      default:
        proto_util::AppendField(field, sched_switch_message);
        break;
    }
  }

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
