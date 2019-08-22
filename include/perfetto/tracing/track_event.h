/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_TRACING_TRACK_EVENT_H_
#define INCLUDE_PERFETTO_TRACING_TRACK_EVENT_H_

#include "perfetto/base/time.h"
#include "perfetto/trace/track_event/track_event.pbzero.h"
#include "perfetto/tracing/internal/track_event_data_source.h"

namespace perfetto {

// Track events are time-based markers that an application can use to construct
// a timeline of its operation.
class TrackEvent {
 public:
  // Initializes the track event data source. Must be called before any other
  // method on this class.
  static void Initialize();

  // Returns the current tracing clock in nanoseconds.
  static uint64_t GetTimeNs() {
    // TODO(skyostil): Consider using boot time where available.
    return static_cast<uint64_t>(perfetto::base::GetWallTimeNs().count());
  }

  // Begin a slice on the current thread. |category| and |name| are free-form
  // strings that describe the event. Both |category| and |name| must be
  // statically allocated.
  static void Begin(const char* category, const char* name) {
    internal::TrackEventDataSource::WriteEvent(
        category, name, perfetto::protos::pbzero::TrackEvent::TYPE_SLICE_BEGIN);
  }

  // End a slice on the current thread.
  static void End(const char* category) {
    internal::TrackEventDataSource::WriteEvent(
        category, nullptr,
        perfetto::protos::pbzero::TrackEvent::TYPE_SLICE_END);
  }

  // TODO(skyostil): Add per-category enable/disable.
  // TODO(skyostil): Add arguments.
  // TODO(skyostil): Add scoped events.
  // TODO(skyostil): Add async events.
  // TODO(skyostil): Add flow events.
  // TODO(skyostil): Add instant events.
  // TODO(skyostil): Add counters.

  static void Flush() {
    internal::TrackEventDataSource::Trace(
        [&](internal::TrackEventDataSource::TraceContext ctx) { ctx.Flush(); });
  }
};

}  // namespace perfetto

PERFETTO_DECLARE_DATA_SOURCE_STATIC_MEMBERS(
    perfetto::internal::TrackEventDataSource,
    perfetto::internal::TrackEventIncrementalState);

#endif  // INCLUDE_PERFETTO_TRACING_TRACK_EVENT_H_
