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

#include "perfetto/tracing/internal/track_event_internal.h"
#include "perfetto/tracing/trace_writer_base.h"
#include "src/shared_lib/track_event/ds.h"

namespace perfetto::shlib {

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
