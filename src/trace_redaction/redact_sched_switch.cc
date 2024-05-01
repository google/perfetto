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

RedactSchedSwitch::RedactSchedSwitch()
    : FtraceEventRedaction(
          protos::pbzero::FtraceEvent::kSchedSwitchFieldNumber) {}

base::Status RedactSchedSwitch::Redact(
    const Context& context,
    const protos::pbzero::FtraceEvent::Decoder& event,
    protozero::ConstBytes bytes,
    protos::pbzero::FtraceEvent* event_message) const {
  if (!context.package_uid.has_value()) {
    return base::ErrStatus("RedactSchedSwitch: missing package uid");
  }

  if (!context.timeline) {
    return base::ErrStatus("RedactSchedSwitch: missing timeline");
  }

  protos::pbzero::SchedSwitchFtraceEvent::Decoder sched_switch(bytes);

  // There must be a prev pid and a next pid. Otherwise, the event is invalid.
  // Dropping the event is the safest option.
  if (!sched_switch.has_prev_pid() || !sched_switch.has_next_pid()) {
    return base::OkStatus();
  }

  // Avoid making the message until we know that we have prev and next pids.
  auto sched_switch_message = event_message->set_sched_switch();

  auto prev_slice =
      context.timeline->Search(event.timestamp(), sched_switch.prev_pid());
  auto next_slice =
      context.timeline->Search(event.timestamp(), sched_switch.next_pid());

  // To read the fields, move the read head back to the start.
  sched_switch.Reset();

  for (auto field = sched_switch.ReadField(); field.valid();
       field = sched_switch.ReadField()) {
    switch (field.id()) {
      case protos::pbzero::SchedSwitchFtraceEvent::kNextCommFieldNumber:
        if (next_slice.uid == context.package_uid) {
          proto_util::AppendField(field, sched_switch_message);
        }
        break;

      case protos::pbzero::SchedSwitchFtraceEvent::kPrevCommFieldNumber:
        if (prev_slice.uid == context.package_uid) {
          proto_util::AppendField(field, sched_switch_message);
        }
        break;

      default:
        proto_util::AppendField(field, sched_switch_message);
        break;
    }
  }

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
