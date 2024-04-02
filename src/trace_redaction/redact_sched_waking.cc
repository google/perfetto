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

#include "src/trace_redaction/redact_sched_waking.h"

#include <string>

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/ftrace/sched.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/trace_redaction/proto_util.h"

namespace perfetto::trace_redaction {

namespace {

// Redact sched waking trace events in a ftrace event bundle:
//
//  event {
//    timestamp: 6702093787823849
//    pid: 814
//    sched_waking {
//      comm: "surfaceflinger"
//      pid: 756
//      prio: 97
//      success: 1
//      target_cpu: 2
//    }
//  }
//
// The three values needed are:
//
//  1. event.pid
//  2. event.timestamp
//  3. event.sched_waking.pid
//
// The two checks that are executed are:
//
//  1. package(event.pid).at(event.timestamp).is(target)
//  2. package(event.sched_waking.pid).at(event.timestamp).is(target)
//
// Both must be true in order to keep an event.
bool KeepEvent(const Context& context, protozero::Field bundle_field) {
  PERFETTO_DCHECK(context.timeline);
  PERFETTO_DCHECK(context.package_uid.has_value());

  PERFETTO_DCHECK(bundle_field.valid());
  PERFETTO_DCHECK(bundle_field.id() ==
                  protos::pbzero::FtraceEventBundle::kEventFieldNumber);

  protozero::ProtoDecoder event_decoder(bundle_field.as_bytes());

  auto sched_waking = event_decoder.FindField(
      protos::pbzero::FtraceEvent::kSchedWakingFieldNumber);

  if (!sched_waking.valid()) {
    return true;
  }

  auto timestamp = event_decoder.FindField(
      protos::pbzero::FtraceEvent::kTimestampFieldNumber);

  if (!timestamp.valid()) {
    return false;
  }

  auto outer_pid =
      event_decoder.FindField(protos::pbzero::FtraceEvent::kPidFieldNumber);

  if (!outer_pid.valid()) {
    return false;
  }

  auto outer_slice = context.timeline->Search(
      timestamp.as_uint64(), static_cast<int32_t>(outer_pid.as_uint32()));

  if (outer_slice.uid != context.package_uid.value()) {
    return false;
  }

  protozero::ProtoDecoder waking_decoder(sched_waking.as_bytes());

  auto inner_pid = waking_decoder.FindField(
      protos::pbzero::SchedWakingFtraceEvent::kPidFieldNumber);

  if (!inner_pid.valid()) {
    return false;
  }

  auto inner_slice =
      context.timeline->Search(timestamp.as_uint64(), inner_pid.as_int32());
  return inner_slice.uid == context.package_uid.value();
}

}  // namespace

base::Status RedactSchedWaking::Transform(const Context& context,
                                          std::string* packet) const {
  if (packet == nullptr || packet->empty()) {
    return base::ErrStatus("RedactSchedWaking: null or empty packet.");
  }

  if (!context.package_uid.has_value()) {
    return base::ErrStatus("RedactSchedWaking: missing packet uid.");
  }

  if (!context.timeline) {
    return base::ErrStatus("RedactSchedWaking: missing timeline.");
  }

  protozero::ProtoDecoder packet_decoder(*packet);

  auto trace_event_bundle = packet_decoder.FindField(
      protos::pbzero::TracePacket::kFtraceEventsFieldNumber);

  if (!trace_event_bundle.valid()) {
    return base::OkStatus();
  }

  protozero::HeapBuffered<protos::pbzero::TracePacket> packet_message;
  packet_message.Reset();

  for (auto packet_field = packet_decoder.ReadField(); packet_field.valid();
       packet_field = packet_decoder.ReadField()) {
    if (packet_field.id() !=
        protos::pbzero::TracePacket::kFtraceEventsFieldNumber) {
      proto_util::AppendField(packet_field, packet_message.get());
      continue;
    }

    protozero::ProtoDecoder bundle_decoder(packet_field.as_bytes());

    auto* bundle_message = packet_message->set_ftrace_events();

    for (auto field = bundle_decoder.ReadField(); field.valid();
         field = bundle_decoder.ReadField()) {
      if (field.id() != protos::pbzero::FtraceEventBundle::kEventFieldNumber ||
          KeepEvent(context, field)) {
        proto_util::AppendField(field, bundle_message);
      }
    }
  }

  *packet = packet_message.SerializeAsString();

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
