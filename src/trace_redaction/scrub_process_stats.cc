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

#include "src/trace_redaction/scrub_process_stats.h"

#include <string>

#include "perfetto/base/status.h"
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/trace_redaction/proto_util.h"
#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/ps/process_stats.pbzero.h"

namespace perfetto::trace_redaction {

base::Status ScrubProcessStats::Transform(const Context& context,
                                          std::string* packet) const {
  if (!context.package_uid.has_value()) {
    return base::ErrStatus("FilterProcessStats: missing package uid.");
  }

  if (!context.timeline) {
    return base::ErrStatus("FilterProcessStats: missing timeline.");
  }

  protozero::ProtoDecoder packet_decoder(*packet);

  // Very few packets will have process stats. It's best to avoid
  // reserialization whenever possible.
  if (!packet_decoder
           .FindField(protos::pbzero::TracePacket::kProcessStatsFieldNumber)
           .valid()) {
    return base::OkStatus();
  }

  protozero::HeapBuffered<protos::pbzero::TracePacket> message;

  // TODO(vaage): Add primitive to drop all packets that don't have a
  // timestamp, allowing all other packets assume there are timestamps.
  auto time_field = packet_decoder.FindField(
      protos::pbzero::TracePacket::kTimestampFieldNumber);
  PERFETTO_DCHECK(time_field.valid());
  auto time = time_field.as_uint64();

  auto* timeline = context.timeline.get();
  auto uid = context.package_uid.value();

  for (auto packet_field = packet_decoder.ReadField(); packet_field.valid();
       packet_field = packet_decoder.ReadField()) {
    if (packet_field.id() !=
        protos::pbzero::TracePacket::kProcessStatsFieldNumber) {
      proto_util::AppendField(packet_field, message.get());
      continue;
    }

    auto process_stats = std::move(packet_field);
    protozero::ProtoDecoder process_stats_decoder(process_stats.as_bytes());

    auto* process_stats_message = message->set_process_stats();

    for (auto process_stats_field = process_stats_decoder.ReadField();
         process_stats_field.valid();
         process_stats_field = process_stats_decoder.ReadField()) {
      bool keep_field;

      if (process_stats_field.id() ==
          protos::pbzero::ProcessStats::kProcessesFieldNumber) {
        protozero::ProtoDecoder process_decoder(process_stats_field.as_bytes());
        auto pid = process_decoder.FindField(
            protos::pbzero::ProcessStats::Process::kPidFieldNumber);
        keep_field =
            pid.valid() && timeline->Search(time, pid.as_int32()).uid == uid;
      } else {
        keep_field = true;
      }

      if (keep_field) {
        proto_util::AppendField(process_stats_field, process_stats_message);
      }
    }
  }

  packet->assign(message.SerializeAsString());

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
