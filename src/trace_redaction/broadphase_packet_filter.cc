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

#include "src/trace_redaction/broadphase_packet_filter.h"

#include "perfetto/base/compiler.h"
#include "perfetto/base/status.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/trace_redaction/proto_util.h"
#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"

namespace perfetto::trace_redaction {
namespace {

// Returns true when every top-level field of `packet` is in `mask` and none is
// `ftrace_events` (which requires inner event filtering via `ftrace_mask`), so
// the packet can pass through untouched without re-serialization.
bool IsPacketFullyAllowed(const std::string& packet,
                          const Context::TracePacketMask& mask) {
  protozero::ProtoDecoder decoder(packet);
  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    if (field.id() >= mask.size() || !mask.test(field.id()) ||
        field.id() == protos::pbzero::TracePacket::kFtraceEventsFieldNumber) {
      return false;
    }
  }
  return true;
}

}  // namespace

base::Status BroadphasePacketFilter::Transform(const Context& context,
                                               std::string* packet) const {
  if (PERFETTO_UNLIKELY(context.packet_mask.none())) {
    return base::ErrStatus("FilterTracePacketFields: empty packet mask.");
  }

  if (PERFETTO_UNLIKELY(!packet || packet->empty())) {
    return base::ErrStatus("FilterTracePacketFields: missing packet.");
  }

  const bool use_app_mask =
      context.app_packet_mask.any() &&
      GetWriterType(protos::pbzero::TracePacket::Decoder(*packet)) ==
          WriterType::kApp;
  const auto& mask =
      use_app_mask ? context.app_packet_mask : context.packet_mask;

  if (PERFETTO_UNLIKELY(
          mask.test(protos::pbzero::TracePacket::kFtraceEventsFieldNumber) &&
          context.ftrace_mask.none())) {
    return base::ErrStatus("FilterTracePacketFields: empty ftrace mask.");
  }

  if (IsPacketFullyAllowed(*packet, mask)) {
    return base::OkStatus();
  }

  protozero::ProtoDecoder decoder(*packet);
  protozero::HeapBuffered<protos::pbzero::TracePacket> message;

  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    // Make sure the id can be references. If it is out of bounds, it is by
    // definition "no set".
    if (field.id() < mask.size() && mask.test(field.id())) {
      if (field.id() == protos::pbzero::TracePacket::kFtraceEventsFieldNumber) {
        OnFtraceEvents(context, field.as_bytes(), message->set_ftrace_events());
      } else {
        proto_util::AppendField(field, message.get());
      }
    }
  }

  packet->assign(message.SerializeAsString());
  return base::OkStatus();
}

void BroadphasePacketFilter::OnFtraceEvents(
    const Context& context,
    protozero::ConstBytes bytes,
    protos::pbzero::FtraceEventBundle* message) const {
  PERFETTO_DCHECK(message);

  protozero::ProtoDecoder decoder(bytes);

  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    if (field.id() == protos::pbzero::FtraceEventBundle::kEventFieldNumber) {
      OnFtraceEvent(context, field.as_bytes(), message->add_event());
    } else {
      proto_util::AppendField(field, message);
    }
  }
}

void BroadphasePacketFilter::OnFtraceEvent(
    const Context& context,
    protozero::ConstBytes bytes,
    protos::pbzero::FtraceEvent* message) const {
  PERFETTO_DCHECK(message);

  protozero::ProtoDecoder decoder(bytes);

  const auto& mask = context.ftrace_mask;

  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    // Make sure the id can be references. If it is out of bounds, it is by
    // definition "no set".
    if (field.id() < mask.size() && mask.test(field.id())) {
      proto_util::AppendField(field, message);
    }
  }
}

}  // namespace perfetto::trace_redaction
