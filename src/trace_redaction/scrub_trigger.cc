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

#include "src/trace_redaction/scrub_trigger.h"

#include "perfetto/base/logging.h"
#include "perfetto/protozero/proto_decoder.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/trace_redaction/proto_util.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/perfetto/trace/trigger.pbzero.h"

namespace perfetto::trace_redaction {
namespace {

using TracePacket = protos::pbzero::TracePacket;
using Trigger = protos::pbzero::Trigger;

}  // namespace

base::Status ScrubTrigger::Transform(const Context& context,
                                     std::string* packet) const {
  // Validated once before Transform() by PackageValidation.
  PERFETTO_DCHECK(context.package_uid.has_value());

  protozero::ProtoDecoder decoder(*packet);
  auto trigger_field = decoder.FindField(TracePacket::kTriggerFieldNumber);
  if (!trigger_field.valid()) {
    return base::OkStatus();
  }

  Trigger::Decoder trigger(trigger_field.as_bytes());
  if (!trigger.has_producer_name() && !trigger.has_trusted_producer_uid()) {
    return base::OkStatus();
  }
  if (trigger.has_trusted_producer_uid() &&
      context.IsTargetUid(trigger.trusted_producer_uid())) {
    return base::OkStatus();
  }

  protozero::HeapBuffered<TracePacket> message;
  decoder.Reset();
  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    if (field.id() != TracePacket::kTriggerFieldNumber) {
      proto_util::AppendField(field, message.get());
      continue;
    }

    auto* scrubbed = message->set_trigger();
    protozero::ProtoDecoder trigger_decoder(field.as_bytes());
    for (auto f = trigger_decoder.ReadField(); f.valid();
         f = trigger_decoder.ReadField()) {
      if (f.id() == Trigger::kProducerNameFieldNumber ||
          f.id() == Trigger::kTrustedProducerUidFieldNumber) {
        continue;
      }
      proto_util::AppendField(f, scrubbed);
    }
  }

  packet->assign(message.SerializeAsString());
  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
