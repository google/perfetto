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

#ifndef SRC_SHARED_LIB_TRACK_EVENT_SERIALIZATION_H_
#define SRC_SHARED_LIB_TRACK_EVENT_SERIALIZATION_H_

#include <stddef.h>
#include <stdint.h>

#include "perfetto/protozero/proto_utils.h"
#include "perfetto/tracing/internal/track_event_internal.h"
#include "perfetto/tracing/trace_writer_base.h"
#include "src/shared_lib/track_event/ds.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::shlib {

// Upper bound of the bytes written by EncodePacketPrologue(): the timestamp
// (1 + 10), the timestamp clock id (2 + 5) and the sequence flags (1 + 5).
constexpr size_t kMaxPacketPrologueSize = 24;

// Encodes the TracePacket fields that every track event packet starts with, in
// this order: timestamp, timestamp_clock_id (if not the default) and
// sequence_flags. For an incremental timestamp it also updates
// `incr_state->last_timestamp_ns`. Returns the end of the encoded bytes.
inline uint8_t* EncodePacketPrologue(
    uint8_t* buf,
    perfetto::shlib::TrackEventIncrementalState* incr_state,
    const perfetto::shlib::TrackEventTlsState& tls_state,
    perfetto::TraceTimestamp timestamp,
    uint32_t seq_flags) {
  using protos::pbzero::TracePacket;
  using protozero::proto_utils::MakeTagVarInt;
  using protozero::proto_utils::WriteVarInt;
  // PERFETTO_TE_TIMESTAMP_TYPE_INCREMENTAL is the default timestamp returned
  // by TrackEventInternal::GetTraceTime(). If the configuration in `tls_state`
  // uses a different clock, we have to use that instead.
  if (PERFETTO_UNLIKELY(tls_state.default_clock_id !=
                            PERFETTO_TE_TIMESTAMP_TYPE_INCREMENTAL &&
                        timestamp.clock_id ==
                            PERFETTO_TE_TIMESTAMP_TYPE_INCREMENTAL)) {
    timestamp.clock_id = tls_state.default_clock_id;
  }
  const uint64_t ts_unit_multiplier = tls_state.timestamp_unit_multiplier;
  uint64_t ts_value;
  bool has_clock_id = false;
  uint32_t clock_id = 0;
  if (PERFETTO_LIKELY(timestamp.clock_id ==
                      PERFETTO_TE_TIMESTAMP_TYPE_INCREMENTAL)) {
    if (PERFETTO_LIKELY(incr_state->last_timestamp_ns <= timestamp.value)) {
      // No need to set the clock id here, since
      // PERFETTO_TE_TIMESTAMP_TYPE_INCREMENTAL is the clock id assumed by
      // default.
      const uint64_t time_diff_ns =
          timestamp.value - incr_state->last_timestamp_ns;
      // The unit multiplier is 1 unless the config sets it. Skip the 64-bit
      // division then.
      if (PERFETTO_LIKELY(ts_unit_multiplier == 1)) {
        ts_value = time_diff_ns;
        incr_state->last_timestamp_ns = timestamp.value;
      } else {
        ts_value = time_diff_ns / ts_unit_multiplier;
        incr_state->last_timestamp_ns += ts_value * ts_unit_multiplier;
      }
    } else {
      ts_value = timestamp.value / ts_unit_multiplier;
      has_clock_id = true;
      clock_id =
          ts_unit_multiplier == 1
              ? PerfettoDsGetDefaultClockId()
              : static_cast<uint32_t>(PERFETTO_TE_TIMESTAMP_TYPE_ABSOLUTE);
    }
  } else if (PERFETTO_LIKELY(timestamp.clock_id ==
                             tls_state.default_clock_id)) {
    ts_value = timestamp.value / ts_unit_multiplier;
  } else {
    ts_value = timestamp.value;
    has_clock_id = true;
    clock_id = timestamp.clock_id;
  }

  buf = WriteVarInt(MakeTagVarInt(TracePacket::kTimestampFieldNumber), buf);
  buf = WriteVarInt(ts_value, buf);
  if (PERFETTO_UNLIKELY(has_clock_id)) {
    buf = WriteVarInt(MakeTagVarInt(TracePacket::kTimestampClockIdFieldNumber),
                      buf);
    buf = WriteVarInt(clock_id, buf);
  }
  buf = WriteVarInt(MakeTagVarInt(TracePacket::kSequenceFlagsFieldNumber), buf);
  return WriteVarInt(seq_flags, buf);
}

// Starts a packet that begins with the fields of EncodePacketPrologue().
protozero::MessageHandle<perfetto::protos::pbzero::TracePacket>
NewTracePacketInternal(perfetto::TraceWriterBase* trace_writer,
                       perfetto::shlib::TrackEventIncrementalState* incr_state,
                       const perfetto::shlib::TrackEventTlsState& tls_state,
                       perfetto::TraceTimestamp timestamp,
                       uint32_t seq_flags);

// Starts a new incremental state sequence on `trace_writer`.
// It emits the packet defaults, the clock snapshot and the thread and process
// track descriptors.
// Only call it when `incr_state->was_cleared` is true.
void ResetIncrementalState(
    perfetto::TraceWriterBase* trace_writer,
    perfetto::shlib::TrackEventIncrementalState* incr_state,
    const perfetto::shlib::TrackEventTlsState& tls_state,
    const perfetto::TraceTimestamp& timestamp);

// Every event calls this, so the check is inline and only the reset is out of
// line.
inline void ResetIncrementalStateIfRequired(
    perfetto::TraceWriterBase* trace_writer,
    perfetto::shlib::TrackEventIncrementalState* incr_state,
    const perfetto::shlib::TrackEventTlsState& tls_state,
    const perfetto::TraceTimestamp& timestamp) {
  if (PERFETTO_UNLIKELY(incr_state->was_cleared))
    ResetIncrementalState(trace_writer, incr_state, tls_state, timestamp);
}

}  // namespace perfetto::shlib

#endif  // SRC_SHARED_LIB_TRACK_EVENT_SERIALIZATION_H_
