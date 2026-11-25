/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_redaction/scrub_service_event.h"

#include "perfetto/protozero/proto_decoder.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/trace_redaction/proto_util.h"

#include "protos/perfetto/trace/perfetto/tracing_service_event.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {
namespace {

using TracePacket = protos::pbzero::TracePacket;
using TracingServiceEvent = protos::pbzero::TracingServiceEvent;

}  // namespace

base::Status ScrubServiceEvent::Transform(const Context&,
                                          std::string* packet) const {
  protozero::ProtoDecoder decoder(*packet);
  auto service_event = decoder.FindField(TracePacket::kServiceEventFieldNumber);
  if (!service_event.valid()) {
    return base::OkStatus();
  }

  auto is_scrubbed_field = [](uint32_t field_id) {
    return field_id ==
               TracingServiceEvent::kSlowStartingDataSourcesFieldNumber ||
           field_id ==
               TracingServiceEvent::kLastFlushSlowDataSourcesFieldNumber;
  };

  protozero::ProtoDecoder event_decoder(service_event.as_bytes());
  bool found_field_to_scrub = false;
  for (auto f = event_decoder.ReadField(); f.valid();
       f = event_decoder.ReadField()) {
    if (is_scrubbed_field(f.id())) {
      found_field_to_scrub = true;
      break;
    }
  }
  if (!found_field_to_scrub) {
    return base::OkStatus();
  }

  protozero::HeapBuffered<TracePacket> message;
  decoder.Reset();
  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    if (field.id() != TracePacket::kServiceEventFieldNumber) {
      proto_util::AppendField(field, message.get());
      continue;
    }
    auto* scrubbed = message->set_service_event();
    protozero::ProtoDecoder fields(field.as_bytes());
    for (auto f = fields.ReadField(); f.valid(); f = fields.ReadField()) {
      if (!is_scrubbed_field(f.id())) {
        proto_util::AppendField(f, scrubbed);
      }
    }
  }

  packet->assign(message.SerializeAsString());
  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
