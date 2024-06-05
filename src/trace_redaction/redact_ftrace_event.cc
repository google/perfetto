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
  protozero::HeapBuffered<protos::pbzero::TracePacket> message;

  protozero::ProtoDecoder decoder(*packet);

  // Treat FtraceEvents (bundle) as a special case.
  for (auto f = decoder.ReadField(); f.valid(); f = decoder.ReadField()) {
    if (f.id() == protos::pbzero::TracePacket::kFtraceEventsFieldNumber) {
      RedactEvents(context, f, message->set_ftrace_events());
    } else {
      proto_util::AppendField(f, message.get());
    }
  }

  packet->assign(message.SerializeAsString());

  return base::OkStatus();
}

// Iterate over every field in FtraceEvents (bundle), treating FtraceEvent as a
// special case (calls the correct redaction).
void RedactFtraceEvent::RedactEvents(
    const Context& context,
    protozero::Field bundle,
    protos::pbzero::FtraceEventBundle* message) const {
  PERFETTO_DCHECK(bundle.id() ==
                  protos::pbzero::TracePacket::kFtraceEventsFieldNumber);

  // There is only one bundle per packet, so creating the bundle decoder is an
  // "okay" expense.
  protos::pbzero::FtraceEventBundle::Decoder bundle_decoder(bundle.as_bytes());

  // Even through we have `bundle_decoder` create a simpler decoder to iterate
  // over every field.
  protozero::ProtoDecoder decoder(bundle.as_bytes());

  // Treat FtraceEvent as a special case.
  for (auto f = decoder.ReadField(); f.valid(); f = decoder.ReadField()) {
    if (f.id() == protos::pbzero::FtraceEventBundle::kEventFieldNumber) {
      RedactEvent(context, bundle_decoder, f, message->add_event());
    } else {
      proto_util::AppendField(f, message);
    }
  }
}

void RedactFtraceEvent::RedactEvent(
    const Context& context,
    const protos::pbzero::FtraceEventBundle::Decoder& bundle,
    protozero::Field event,
    protos::pbzero::FtraceEvent* message) const {
  PERFETTO_DCHECK(event.id() ==
                  protos::pbzero::FtraceEventBundle::kEventFieldNumber);

  // A modifier can/will change the decoder by calling ReadField(). To avoid a
  // modifier from interfering with the this function's loop, a reusable decoder
  // is used for each modifier call.
  protozero::ProtoDecoder outer_decoder(event.as_bytes());
  protozero::ProtoDecoder inner_decoder(event.as_bytes());

  // If there is a handler for a field, treat it as a special case.
  for (auto f = outer_decoder.ReadField(); f.valid();
       f = outer_decoder.ReadField()) {
    auto* mod = redactions_.Find(f.id());
    if (mod && mod->get()) {
      // Reset the decoder so that it appears like a "new" decoder to the
      // modifier.
      inner_decoder.Reset();
      mod->get()->Redact(context, bundle, inner_decoder, message);
    } else {
      proto_util::AppendField(f, message);
    }
  }
}
}  // namespace perfetto::trace_redaction
