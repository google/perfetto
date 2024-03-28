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

#include "src/trace_redaction/scrub_task_rename.h"

#include <string>

#include "perfetto/base/status.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/trace_redaction/proto_util.h"
#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"

namespace perfetto::trace_redaction {

namespace {

bool ShouldKeepField(const Context& context, protozero::Field event) {
  PERFETTO_DCHECK(event.id() ==
                  protos::pbzero::FtraceEventBundle::kEventFieldNumber);

  protozero::ProtoDecoder event_decoder(event.as_bytes());

  protozero::Field pid = {};
  protozero::Field timestamp = {};
  protozero::Field rename = {};

  for (auto event_field = event_decoder.ReadField(); event_field.valid();
       event_field = event_decoder.ReadField()) {
    switch (event_field.id()) {
      case protos::pbzero::FtraceEvent::kPidFieldNumber:
        pid = event_field;
        break;

      case protos::pbzero::FtraceEvent::kTimestampFieldNumber:
        timestamp = event_field;
        break;

      case protos::pbzero::FtraceEvent::kTaskRenameFieldNumber:
        rename = event_field;
        break;

      default:
        break;
    }
  }

  if (rename.valid() && timestamp.valid() && pid.valid()) {
    auto slice =
        context.timeline->Search(timestamp.as_uint64(), pid.as_int32());
    return slice.uid == context.package_uid.value();
  }

  // If there is a rename field, but the time is invalid and/or the pid is
  // invalid, be defensive and throw the event away.
  if (rename.valid()) {
    return false;
  }

  return true;
}

}  // namespace

base::Status ScrubTaskRename::Transform(const Context& context,
                                        std::string* packet) const {
  if (!context.package_uid.has_value()) {
    return base::ErrStatus("ScrubTaskRename: missing package uid.");
  }

  if (!context.timeline) {
    return base::ErrStatus("ScrubTaskRename: missing timeline.");
  }

  // Check if there is a ftrace event bundle field.
  protozero::ProtoDecoder packet_decoder(*packet);
  auto ftrace_event_bundle = packet_decoder.FindField(
      protos::pbzero::TracePacket::kFtraceEventsFieldNumber);

  if (!ftrace_event_bundle.valid()) {
    return base::OkStatus();
  }

  protozero::HeapBuffered<protos::pbzero::TracePacket> packet_msg;

  // Some decoders will be re-used.
  packet_decoder.Reset();

  for (auto packet_field = packet_decoder.ReadField(); packet_field.valid();
       packet_field = packet_decoder.ReadField()) {
    if (packet_field.id() !=
        protos::pbzero::TracePacket::kFtraceEventsFieldNumber) {
      proto_util::AppendField(packet_field, packet_msg.get());
      continue;
    }

    auto* bundle_msg = packet_msg->set_ftrace_events();

    protozero::ProtoDecoder bundle_decoder(packet_field.as_bytes());

    for (auto bundle_field = bundle_decoder.ReadField(); bundle_field.valid();
         bundle_field = bundle_decoder.ReadField()) {
      bool keep_field = false;

      if (bundle_field.id() ==
          protos::pbzero::FtraceEventBundle::kEventFieldNumber) {
        keep_field = ShouldKeepField(context, bundle_field);
      } else {
        keep_field = true;
      }

      if (keep_field) {
        proto_util::AppendField(bundle_field, bundle_msg);
      }
    }
  }

  packet->assign(packet_msg.SerializeAsString());

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
