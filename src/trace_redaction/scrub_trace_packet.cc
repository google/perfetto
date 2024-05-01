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

#include "src/trace_redaction/scrub_trace_packet.h"

#include "perfetto/base/status.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/trace_processor/util/status_macros.h"
#include "src/trace_redaction/proto_util.h"

namespace perfetto::trace_redaction {

TracePacketFilter::~TracePacketFilter() = default;

base::Status TracePacketFilter::VerifyContext(const Context&) const {
  return base::OkStatus();
}

base::Status ScrubTracePacket::Transform(const Context& context,
                                         std::string* packet) const {
  if (packet == nullptr || packet->empty()) {
    return base::ErrStatus("ScrubTracePacket: null or empty packet.");
  }

  for (const auto& filter : filters_) {
    RETURN_IF_ERROR(filter->VerifyContext(context));
  }

  protozero::HeapBuffered<protos::pbzero::TracePacket> new_packet;

  protozero::ProtoDecoder decoder(*packet);

  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    if (KeepEvent(context, field)) {
      proto_util::AppendField(field, new_packet.get());
    }
  }

  packet->assign(new_packet.SerializeAsString());
  return base::OkStatus();
}

// Logical AND all filters.
bool ScrubTracePacket::KeepEvent(const Context& context,
                                 const protozero::Field& field) const {
  for (const auto& filter : filters_) {
    if (!filter->KeepField(context, field)) {
      return false;
    }
  }

  return true;
}

}  // namespace perfetto::trace_redaction
