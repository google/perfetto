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

#include <string>

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/ftrace/sched.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/trace_redaction/proto_util.h"

namespace perfetto::trace_redaction {

namespace {

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
void RedactSwitchEvent(
    const Context& context,
    protos::pbzero::FtraceEvent::Decoder event,
    protos::pbzero::FtraceEventBundle* ftrace_event_bundle_message) {
  PERFETTO_DCHECK(context.timeline);
  PERFETTO_DCHECK(context.package_uid.has_value());
  PERFETTO_DCHECK(event.has_sched_switch());
  PERFETTO_DCHECK(ftrace_event_bundle_message);

  // If there is no timestamp in the event, it is not possible to query the
  // timeline. This is too risky to keep.
  if (!event.has_timestamp()) {
    return;
  }

  protos::pbzero::SchedSwitchFtraceEvent::Decoder sched_switch(
      event.sched_switch());

  // There must be a prev pid and a next pid. Otherwise, the event is invalid.
  // Dropping the event is the safest option.
  if (!sched_switch.has_prev_pid() || !sched_switch.has_next_pid()) {
    return;
  }

  auto uid = context.package_uid.value();

  auto prev_slice =
      context.timeline->Search(event.timestamp(), sched_switch.prev_pid());
  auto next_slice =
      context.timeline->Search(event.timestamp(), sched_switch.next_pid());

  // Build a new event, clearing the comm values when needed.
  auto* event_message = ftrace_event_bundle_message->add_event();

  // Reset to scan event fields.
  event.Reset();

  for (auto event_field = event.ReadField(); event_field.valid();
       event_field = event.ReadField()) {
    // This primitive only needs to affect sched switch events.
    if (event_field.id() !=
        protos::pbzero::FtraceEvent::kSchedSwitchFieldNumber) {
      proto_util::AppendField(event_field, event_message);
      continue;
    }

    // Reset to scan sched_switch fields.
    sched_switch.Reset();

    auto switch_message = event_message->set_sched_switch();

    for (auto switch_field = sched_switch.ReadField(); switch_field.valid();
         switch_field = sched_switch.ReadField()) {
      switch (switch_field.id()) {
        case protos::pbzero::SchedSwitchFtraceEvent::kPrevCommFieldNumber:
          if (prev_slice.uid == uid) {
            proto_util::AppendField(switch_field, switch_message);
          }
          break;

        case protos::pbzero::SchedSwitchFtraceEvent::kNextCommFieldNumber: {
          if (next_slice.uid == uid) {
            proto_util::AppendField(switch_field, switch_message);
          }
          break;
        }

        default:
          proto_util::AppendField(switch_field, switch_message);
          break;
      }
    }
  }
}

}  // namespace

base::Status RedactSchedSwitch::Transform(const Context& context,
                                          std::string* packet) const {
  if (packet == nullptr || packet->empty()) {
    return base::ErrStatus("RedactSchedSwitch: null or empty packet.");
  }

  if (!context.package_uid.has_value()) {
    return base::ErrStatus("RedactSchedSwitch: missing packet uid.");
  }

  if (!context.timeline) {
    return base::ErrStatus("RedactSchedSwitch: missing timeline.");
  }

  protozero::ProtoDecoder packet_decoder(*packet);

  auto trace_event_bundle = packet_decoder.FindField(
      protos::pbzero::TracePacket::kFtraceEventsFieldNumber);

  if (!trace_event_bundle.valid()) {
    return base::OkStatus();
  }

  protozero::HeapBuffered<protos::pbzero::TracePacket> packet_message;

  for (auto packet_field = packet_decoder.ReadField(); packet_field.valid();
       packet_field = packet_decoder.ReadField()) {
    if (packet_field.id() !=
        protos::pbzero::TracePacket::kFtraceEventsFieldNumber) {
      proto_util::AppendField(packet_field, packet_message.get());
      continue;
    }

    protozero::ProtoDecoder bundle(packet_field.as_bytes());

    auto* bundle_message = packet_message->set_ftrace_events();

    for (auto field = bundle.ReadField(); field.valid();
         field = bundle.ReadField()) {
      if (field.id() != protos::pbzero::FtraceEventBundle::kEventFieldNumber) {
        proto_util::AppendField(field, bundle_message);
        continue;
      }

      protos::pbzero::FtraceEvent::Decoder ftrace_event(field.as_bytes());

      if (ftrace_event.has_sched_switch()) {
        RedactSwitchEvent(context, std::move(ftrace_event), bundle_message);
      } else {
        proto_util::AppendField(field, bundle_message);
      }
    }
  }

  *packet = packet_message.SerializeAsString();

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
