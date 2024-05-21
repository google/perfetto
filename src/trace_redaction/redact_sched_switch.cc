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

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/trace_processor/util/status_macros.h"
#include "src/trace_redaction/proto_util.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/ftrace/sched.pbzero.h"

namespace perfetto::trace_redaction {

namespace {
// TODO(vaage): While simple, this function saves us from declaring the sample
// lambda each time we use the has_fields pattern. Once its usage increases, and
// its value is obvious, remove this comment.
bool IsTrue(bool value) {
  return value;
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

SchedSwitchTransform::~SchedSwitchTransform() = default;

base::Status RedactSchedSwitchHarness::Transform(const Context& context,
                                                 std::string* packet) const {
  protozero::HeapBuffered<protos::pbzero::TracePacket> message;
  protozero::ProtoDecoder decoder(*packet);

  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    if (field.id() == protos::pbzero::TracePacket::kFtraceEventsFieldNumber) {
      RETURN_IF_ERROR(
          TransformFtraceEvents(context, field, message->set_ftrace_events()));
    } else {
      proto_util::AppendField(field, message.get());
    }
  }

  packet->assign(message.SerializeAsString());

  return base::OkStatus();
}

base::Status RedactSchedSwitchHarness::TransformFtraceEvents(
    const Context& context,
    protozero::Field ftrace_events,
    protos::pbzero::FtraceEventBundle* message) const {
  PERFETTO_DCHECK(ftrace_events.id() ==
                  protos::pbzero::TracePacket::kFtraceEventsFieldNumber);

  protozero::ProtoDecoder decoder(ftrace_events.as_bytes());

  auto cpu =
      decoder.FindField(protos::pbzero::FtraceEventBundle::kCpuFieldNumber);
  if (!cpu.valid()) {
    return base::ErrStatus(
        "RedactSchedSwitchHarness: missing cpu in ftrace event bundle.");
  }

  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    if (field.id() == protos::pbzero::FtraceEventBundle::kEventFieldNumber) {
      RETURN_IF_ERROR(TransformFtraceEvent(context, cpu.as_int32(), field,
                                           message->add_event()));
      continue;
    }

    if (field.id() ==
        protos::pbzero::FtraceEventBundle::kCompactSchedFieldNumber) {
      // TODO(vaage): Replace this with logic specific to the comp sched data
      // type.
      proto_util::AppendField(field, message);
      continue;
    }

    proto_util::AppendField(field, message);
  }

  return base::OkStatus();
}

base::Status RedactSchedSwitchHarness::TransformFtraceEvent(
    const Context& context,
    int32_t cpu,
    protozero::Field ftrace_event,
    protos::pbzero::FtraceEvent* message) const {
  PERFETTO_DCHECK(ftrace_event.id() ==
                  protos::pbzero::FtraceEventBundle::kEventFieldNumber);

  protozero::ProtoDecoder decoder(ftrace_event.as_bytes());

  auto ts =
      decoder.FindField(protos::pbzero::FtraceEvent::kTimestampFieldNumber);
  if (!ts.valid()) {
    return base::ErrStatus(
        "RedactSchedSwitchHarness: missing timestamp in ftrace event.");
  }

  std::string scratch_str;

  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    if (field.id() == protos::pbzero::FtraceEvent::kSchedSwitchFieldNumber) {
      protos::pbzero::SchedSwitchFtraceEvent::Decoder sched_switch(
          field.as_bytes());
      RETURN_IF_ERROR(TransformFtraceEventSchedSwitch(
          context, ts.as_uint64(), cpu, sched_switch, &scratch_str,
          message->set_sched_switch()));
    } else {
      proto_util::AppendField(field, message);
    }
  }

  return base::OkStatus();
}

base::Status RedactSchedSwitchHarness::TransformFtraceEventSchedSwitch(
    const Context& context,
    uint64_t ts,
    int32_t cpu,
    protos::pbzero::SchedSwitchFtraceEvent::Decoder& sched_switch,
    std::string* scratch_str,
    protos::pbzero::SchedSwitchFtraceEvent* message) const {
  auto has_fields = {
      sched_switch.has_prev_comm(), sched_switch.has_prev_pid(),
      sched_switch.has_prev_prio(), sched_switch.has_prev_state(),
      sched_switch.has_next_comm(), sched_switch.has_next_pid(),
      sched_switch.has_next_prio()};

  if (!std::all_of(has_fields.begin(), has_fields.end(), IsTrue)) {
    return base::ErrStatus(
        "RedactSchedSwitchHarness: missing required SchedSwitchFtraceEvent "
        "field.");
  }

  auto prev_pid = sched_switch.prev_pid();
  auto prev_comm = sched_switch.prev_comm();

  auto next_pid = sched_switch.next_pid();
  auto next_comm = sched_switch.next_comm();

  // There are 7 values in a sched switch message. Since 4 of the 7 can be
  // replaced, it is easier/cleaner to go value-by-value. Go in proto-defined
  // order.

  scratch_str->assign(prev_comm.data, prev_comm.size);

  for (const auto& transform : transforms_) {
    RETURN_IF_ERROR(
        transform->Transform(context, ts, cpu, &prev_pid, scratch_str));
  }

  message->set_prev_comm(*scratch_str);                // FieldNumber = 1
  message->set_prev_pid(prev_pid);                     // FieldNumber = 2
  message->set_prev_prio(sched_switch.prev_prio());    // FieldNumber = 3
  message->set_prev_state(sched_switch.prev_state());  // FieldNumber = 4

  scratch_str->assign(next_comm.data, next_comm.size);

  for (const auto& transform : transforms_) {
    RETURN_IF_ERROR(
        transform->Transform(context, ts, cpu, &next_pid, scratch_str));
  }

  message->set_next_comm(*scratch_str);              // FieldNumber = 5
  message->set_next_pid(next_pid);                   // FieldNumber = 6
  message->set_next_prio(sched_switch.next_prio());  // FieldNumber = 7

  return base::OkStatus();
}

// Switch event transformation: Clear the comm value if the thread/process is
// not part of the target packet.
base::Status ClearComms::Transform(const Context& context,
                                   uint64_t ts,
                                   int32_t,
                                   int32_t* pid,
                                   std::string* comm) const {
  PERFETTO_DCHECK(pid);
  PERFETTO_DCHECK(comm);

  if (!context.timeline->PidConnectsToUid(ts, *pid, *context.package_uid)) {
    comm->clear();
  }

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
