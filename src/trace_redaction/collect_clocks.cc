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

#include "src/trace_redaction/collect_clocks.h"

#include "perfetto/protozero/field.h"

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/clock_snapshot.pbzero.h"
#include "protos/perfetto/trace/trace_packet_defaults.pbzero.h"

using namespace perfetto::trace_processor;

namespace perfetto::trace_redaction {

base::Status CollectClocks::Collect(
    const protos::pbzero::TracePacket::Decoder& packet,
    Context* context) const {
  context->clock_snapshot.clear();
  if (packet.has_clock_snapshot()) {
    protos::pbzero::ClockSnapshot_Decoder snapshot_decoder(
        packet.clock_snapshot());

    if (snapshot_decoder.has_primary_trace_clock()) {
      int32_t trace_clock = snapshot_decoder.primary_trace_clock();
      context->clock_converter.SetPrimaryTraceClock(
          static_cast<int64_t>(trace_clock));
    }
    for (auto clock_it = snapshot_decoder.clocks(); clock_it; clock_it++) {
      RedactorClockSynchronizer::ClockTimestamp clock_ts(0, 0);

      protos::pbzero::ClockSnapshot_Clock_Decoder clock_decoder(
          clock_it->as_bytes());

      if (clock_decoder.has_clock_id()) {
        RedactorClockSynchronizer::Clock clock =
            RedactorClockSynchronizer::Clock(
                static_cast<int64_t>(clock_decoder.clock_id()));
        clock_ts.clock = clock;
      }

      if (clock_decoder.has_timestamp()) {
        clock_ts.timestamp = static_cast<int64_t>(clock_decoder.timestamp());
      }
      context->clock_snapshot.push_back(clock_ts);
    }

    RETURN_IF_ERROR(
        context->clock_converter.AddClockSnapshot(context->clock_snapshot));
  } else if (packet.has_trace_packet_defaults()) {
    RETURN_IF_ERROR(
        OnTracePacketDefaults(packet.trace_packet_defaults(), context));
  }

  return base::OkStatus();
}

base::Status CollectClocks::OnTracePacketDefaults(
    protozero::ConstBytes trace_packet_defaults_bytes,
    Context* context) const {
  protos::pbzero::TracePacketDefaults_Decoder trace_packet_defaults_decoder(
      trace_packet_defaults_bytes);
  if (trace_packet_defaults_decoder.has_timestamp_clock_id()) {
    uint32_t perf_clock_id = trace_packet_defaults_decoder.timestamp_clock_id();
    context->clock_converter.SetPerfTraceClock(perf_clock_id);
    return base::OkStatus();
  }

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
