/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_redaction/prune_perf_events.h"

#include "perfetto/protozero/field.h"
#include "perfetto/protozero/scattered_heap_buffer.h"

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "src/trace_redaction/proto_util.h"
#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/profiling/profile_packet.pbzero.h"

using namespace perfetto::trace_processor;
namespace perfetto::trace_redaction {

base::Status PrunePerfEvents::Transform(const Context& context,
                                        std::string* packet) const {
  if (packet == nullptr || packet->empty()) {
    return base::ErrStatus("PrunePerfEvents: null or empty packet.");
  }

  protozero::ProtoDecoder decoder(*packet);

  auto perf_sample =
      decoder.FindField(protos::pbzero::TracePacket::kPerfSampleFieldNumber);
  if (!perf_sample.valid()) {
    // No perf samples found, skip.
    return base::OkStatus();
  }

  protozero::HeapBuffered<protos::pbzero::TracePacket> message;

  auto time_field =
      decoder.FindField(protos::pbzero::TracePacket::kTimestampFieldNumber);
  PERFETTO_DCHECK(time_field.valid());

  auto ts = time_field.as_uint64();

  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    if (field.id() == protos::pbzero::TracePacket::kPerfSampleFieldNumber) {
      RETURN_IF_ERROR(OnPerfSample(context, ts, field, message.get()));
    } else {
      proto_util::AppendField(field, message.get());
    }
  }

  packet->assign(message.SerializeAsString());

  return base::OkStatus();
}

base::Status PrunePerfEvents::OnPerfSample(
    const Context& context,
    uint64_t ts,
    protozero::Field& perf_sample_field,
    protos::pbzero::TracePacket* message) const {
  protozero::ProtoDecoder decoder(perf_sample_field.as_bytes());

  auto pid = decoder.FindField(protos::pbzero::PerfSample::kPidFieldNumber);
  PERFETTO_DCHECK(pid.valid());

  // Performance samples tend to use a different clock, most of the time
  // CLOCK_MONOTONIC_RAW while the Timeline uses the trace clock which tends to
  // be CLOCK_BOOTTIME so we have to make sure perf events are converted to the
  // same time domain as the timeline.
  uint64_t trace_ts;
  RETURN_IF_ERROR(context.clock_converter.ConvertPerfToTrace(ts, &trace_ts));

  if (filter_->Includes(context, trace_ts, pid.as_int32())) {
    proto_util::AppendField(perf_sample_field, message);
  }

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
