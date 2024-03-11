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

#include "src/trace_redaction/scrub_ftrace_events.h"

#include <string>

#include "perfetto/protozero/message.h"
#include "perfetto/protozero/message_arena.h"
#include "perfetto/protozero/scattered_heap_buffer.h"

#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"

namespace perfetto::trace_redaction {
namespace {

constexpr auto kFtraceEventsFieldNumber =
    protos::pbzero::TracePacket::kFtraceEventsFieldNumber;

constexpr auto kEventFieldNumber =
    protos::pbzero::FtraceEventBundle::kEventFieldNumber;

enum class Redact : uint8_t {
  // Some resources in the target need to be redacted.
  kSomething = 0,

  // No resources in the target need to be redacted.
  kNothing = 1,
};

// Return kSomething if an event will change after redaction . If a packet
// will not change, then the packet should skip redaction and be appended
// to the output.
//
// Event packets have few packets (e.g. timestamp, pid, the event payload).
// because of this, it is relatively cheap to test a packet.
//
//  event {
//    timestamp: 6702095044306682
//    pid: 0
//    sched_switch {
//      prev_comm: "swapper/2"
//      prev_pid: 0
//      prev_prio: 120
//      prev_state: 0
//      next_comm: "surfaceflinger"
//      next_pid: 819
//      next_prio: 120
//    }
//  }
Redact ProbeEvent(const Context& context, const protozero::Field& event) {
  if (event.id() != kEventFieldNumber) {
    PERFETTO_FATAL("Invalid proto field. Expected kEventFieldNumber.");
  }

  protozero::ProtoDecoder decoder(event.data(), event.size());

  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    if (context.ftrace_packet_allow_list.count(field.id()) != 0) {
      return Redact::kNothing;
    }
  }

  return Redact::kSomething;
}

}  // namespace

//  packet {
//    ftrace_events {
//      event {                   <-- This is where we test the allow-list
//        timestamp: 6702095044299807
//        pid: 0
//        cpu_idle {              <-- This is the event data (allow-list)
//          state: 4294967295
//          cpu_id: 2
//        }
//      }
//    }
//  }
base::Status ScrubFtraceEvents::Transform(const Context& context,
                                          std::string* packet) const {
  if (packet == nullptr || packet->empty()) {
    return base::ErrStatus("Cannot scrub null or empty trace packet.");
  }

  if (context.ftrace_packet_allow_list.empty()) {
    return base::ErrStatus("Cannot scrub ftrace packets, missing allow-list.");
  }

  // If the packet has no ftrace events, skip it, leaving it unmodified.
  protos::pbzero::TracePacket::Decoder query(*packet);
  if (!query.has_ftrace_events()) {
    return base::OkStatus();
  }

  protozero::HeapBuffered<protos::pbzero::TracePacket> packet_msg;

  // packet.foreach_child.foreach( ... )
  protozero::ProtoDecoder d_packet(*packet);
  for (auto packet_child_it = d_packet.ReadField(); packet_child_it.valid();
       packet_child_it = d_packet.ReadField()) {
    // packet.child_not<ftrace_events>( ).do ( ... )
    if (packet_child_it.id() != kFtraceEventsFieldNumber) {
      AppendField(packet_child_it, packet_msg.get());
      continue;
    }

    // To clarify, "ftrace_events" is the field name and "FtraceEventBundle" is
    // the field type. The terms are often used interchangeably.
    auto* ftrace_events_msg = packet_msg->set_ftrace_events();

    // packet.child<ftrace_events>( ).foreach_child( ... )
    protozero::ProtoDecoder ftrace_events(packet_child_it.as_bytes());
    for (auto ftrace_events_it = ftrace_events.ReadField();
         ftrace_events_it.valid();
         ftrace_events_it = ftrace_events.ReadField()) {
      // packet.child<ftrace_events>( ).child_not<event>( ).do ( ... )
      if (ftrace_events_it.id() != kEventFieldNumber) {
        AppendField(ftrace_events_it, ftrace_events_msg);
        continue;
      }

      // packet.child<ftrace_events>( ).child_is<event>( ).do ( ... )
      if (ProbeEvent(context, ftrace_events_it) == Redact::kNothing) {
        AppendField(ftrace_events_it, ftrace_events_msg);
        continue;
      }

      // Dropping packet = "is event" and "is redacted"
    }
  }

  packet->assign(packet_msg.SerializeAsString());
  return base::OkStatus();
}

// This is copied from "src/protozero/field.cc", but was modified to use the
// serialization methods provided in "perfetto/protozero/message.h".
void ScrubFtraceEvents::AppendField(const protozero::Field& field,
                                    protozero::Message* message) {
  auto id = field.id();
  auto type = field.type();

  switch (type) {
    case protozero::proto_utils::ProtoWireType::kVarInt: {
      message->AppendVarInt(id, field.raw_int_value());
      return;
    }

    case protozero::proto_utils::ProtoWireType::kFixed32: {
      message->AppendFixed(id, field.as_uint32());
      return;
    }

    case protozero::proto_utils::ProtoWireType::kFixed64: {
      message->AppendFixed(id, field.as_uint64());
      return;
    }

    case protozero::proto_utils::ProtoWireType::kLengthDelimited: {
      message->AppendBytes(id, field.data(), field.size());
      return;
    }
  }

  // A switch-statement would be preferred, but when using a switch statement,
  // it complains that about case coverage.
  PERFETTO_FATAL("Unknown field type %u", static_cast<uint8_t>(type));
}

}  // namespace perfetto::trace_redaction
