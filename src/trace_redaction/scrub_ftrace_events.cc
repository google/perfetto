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

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/trace_redaction/proto_util.h"

#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"

namespace perfetto::trace_redaction {

FtraceEventFilter::~FtraceEventFilter() = default;

//  packet {
//    ftrace_events {
//      event {                   <-- This is where we test the allow-list
//        timestamp: 6702095044299807
//        pid: 0
//        cpu_idle {              <-- This is the event type
//          state: 4294967295
//          cpu_id: 2
//        }
//      }
//    }
//  }
base::Status ScrubFtraceEvents::Transform(const Context& context,
                                          std::string* packet) const {
  if (packet == nullptr || packet->empty()) {
    return base::ErrStatus("ScrubFtraceEvents: null or empty packet.");
  }

  for (const auto& filter : filters_) {
    auto status = filter->VerifyContext(context);

    if (!status.ok()) {
      return status;
    }
  }

  protozero::ProtoDecoder packet_decoder(*packet);

  if (!packet_decoder
           .FindField(protos::pbzero::TracePacket::kFtraceEventsFieldNumber)
           .valid()) {
    return base::OkStatus();
  }

  protozero::HeapBuffered<protos::pbzero::TracePacket> packet_message;

  for (auto field = packet_decoder.ReadField(); field.valid();
       field = packet_decoder.ReadField()) {
    if (field.id() != protos::pbzero::TracePacket::kFtraceEventsFieldNumber) {
      proto_util::AppendField(field, packet_message.get());
      continue;
    }

    auto* bundle_message = packet_message->set_ftrace_events();

    protozero::ProtoDecoder bundle(field.as_bytes());

    for (auto event_it = bundle.ReadField(); event_it.valid();
         event_it = bundle.ReadField()) {
      if (event_it.id() !=
              protos::pbzero::FtraceEventBundle::kEventFieldNumber ||
          KeepEvent(context, event_it.as_bytes())) {
        proto_util::AppendField(event_it, bundle_message);
      }
    }
  }

  packet->assign(packet_message.SerializeAsString());

  return base::OkStatus();
}

// Logical AND of all filters.
bool ScrubFtraceEvents::KeepEvent(const Context& context,
                                  protozero::ConstBytes bytes) const {
  for (const auto& filter : filters_) {
    auto keep = filter->KeepEvent(context, bytes);

    if (!keep) {
      return false;
    }
  }

  return true;
}

}  // namespace perfetto::trace_redaction
