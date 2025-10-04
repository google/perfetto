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

  auto trace_packet_clock_id_field = decoder.FindField(
      protos::pbzero::TracePacket::kTimestampClockIdFieldNumber);
  int64_t trace_packet_clock_id = -1;
  int64_t trusted_packet_sequence_id = -1;
  if (PERFETTO_UNLIKELY(trace_packet_clock_id_field.valid())) {
    // A clock id was overriden for the packet.
    trace_packet_clock_id = trace_packet_clock_id_field.as_uint32();
  } else {
    // No clock if provided, we need to use the trace defaults. Find the
    // corresponding trusted sequence id to identify the correct clock.
    auto trusted_packet_sequence_id_field = decoder.FindField(
        protos::pbzero::TracePacket::kTrustedPacketSequenceIdFieldNumber);
    if (trusted_packet_sequence_id_field.valid()) {
      trusted_packet_sequence_id = trusted_packet_sequence_id_field.as_int64();
    }
  }

  auto ts = time_field.as_uint64();

  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    if (field.id() == protos::pbzero::TracePacket::kPerfSampleFieldNumber) {
      RETURN_IF_ERROR(OnPerfSample(context, ts, trace_packet_clock_id,
                                   trusted_packet_sequence_id, field,
                                   message.get()));
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
    int64_t trace_packet_clock_id,
    int64_t trusted_packet_sequence_id,
    protozero::Field& perf_sample_field,
    protos::pbzero::TracePacket* message) const {
  protozero::ProtoDecoder decoder(perf_sample_field.as_bytes());

  auto pid = decoder.FindField(protos::pbzero::PerfSample::kPidFieldNumber);
  PERFETTO_DCHECK(pid.valid());

  uint64_t trace_ts;
  ClockId clock_id = trace_packet_clock_id;
  if (PERFETTO_LIKELY(trace_packet_clock_id == -1)) {
    // No override provided, so grab the default clock for this sequence id.
    PERFETTO_DCHECK(trusted_packet_sequence_id != -1);
    ASSIGN_OR_RETURN(
        clock_id, context.clock_converter.GetDataSourceClock(
                      static_cast<uint32_t>(trusted_packet_sequence_id),
                      RedactorClockConverter::DataSourceType::kPerfDataSource));
  }

  ASSIGN_OR_RETURN(trace_ts,
                   context.clock_converter.ConvertToTrace(clock_id, ts));

  if (filter_->Includes(context, trace_ts, pid.as_int32())) {
    proto_util::AppendField(perf_sample_field, message);
  }

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
