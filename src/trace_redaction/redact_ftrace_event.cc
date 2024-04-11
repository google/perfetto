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

#include <string>

#include "perfetto/base/status.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/trace_redaction/proto_util.h"
#include "src/trace_redaction/redact_ftrace_event.h"
#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"

namespace perfetto::trace_redaction {

FtraceEventRedaction::~FtraceEventRedaction() = default;

base::Status RedactFtraceEvent::Transform(const Context& context,
                                          std::string* packet) const {
  protozero::ConstBytes packet_bytes = {
      reinterpret_cast<const uint8_t*>(packet->data()), packet->size()};

  protozero::HeapBuffered<protos::pbzero::TracePacket> message;

  RedactPacket(context, packet_bytes, message.get());
  packet->assign(message.SerializeAsString());

  return base::OkStatus();
}

// Iterate over every field in a packet, treating FtraceEvents (bundle) as a
// special case.
void RedactFtraceEvent::RedactPacket(
    const Context& context,
    protozero::ConstBytes bytes,
    protos::pbzero::TracePacket* message) const {
  protozero::ProtoDecoder decoder(bytes);

  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    if (field.id() == protos::pbzero::TracePacket::kFtraceEventsFieldNumber) {
      RedactEvents(context, field.as_bytes(), message->set_ftrace_events());
    } else {
      proto_util::AppendField(field, message);
    }
  }
}

// Iterate over every field in FtraceEvents (bundle), treating FtraceEvent as a
// special case (calls the correct redaction).
void RedactFtraceEvent::RedactEvents(
    const Context& context,
    protozero::ConstBytes bytes,
    protos::pbzero::FtraceEventBundle* message) const {
  protozero::ProtoDecoder decoder(bytes);

  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    if (field.id() == protos::pbzero::FtraceEventBundle::kEventFieldNumber) {
      RedactEvent(context, field.as_bytes(), message->add_event());
    } else {
      proto_util::AppendField(field, message);
    }
  }
}

void RedactFtraceEvent::RedactEvent(
    const Context& context,
    protozero::ConstBytes bytes,
    protos::pbzero::FtraceEvent* message) const {
  protozero::ProtoDecoder event(bytes);

  for (auto field = event.ReadField(); field.valid();
       field = event.ReadField()) {
    auto mod = FindRedactionFor(field.id());

    if (mod) {
      protos::pbzero::FtraceEvent::Decoder event_decoder(bytes);
      mod->Redact(context, event_decoder, field.as_bytes(), message);
    } else {
      proto_util::AppendField(field, message);
    }
  }
}

const FtraceEventRedaction* RedactFtraceEvent::FindRedactionFor(
    uint32_t i) const {
  for (const auto& modification : redactions_) {
    if (modification->field_id() == i) {
      return modification.get();
    }
  }

  return nullptr;
}

}  // namespace perfetto::trace_redaction
