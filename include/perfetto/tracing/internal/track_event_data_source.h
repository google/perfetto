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

#ifndef INCLUDE_PERFETTO_TRACING_INTERNAL_TRACK_EVENT_DATA_SOURCE_H_
#define INCLUDE_PERFETTO_TRACING_INTERNAL_TRACK_EVENT_DATA_SOURCE_H_

#include "perfetto/tracing/data_source.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"

#include <unordered_map>

namespace perfetto {
class TrackEvent;

namespace internal {

struct TrackEventIncrementalState {
  bool was_cleared = true;

  // Interned data.
  // TODO(skyostil): Replace this with something more clever that supports
  // dynamic strings too.
  std::unordered_map<const char*, uint64_t> event_names;
  std::unordered_map<const char*, uint64_t> categories;
};

class TrackEventDataSource
    : public DataSource<TrackEventDataSource, TrackEventIncrementalState> {
 public:
  void OnSetup(const SetupArgs&) override;
  void OnStart(const StartArgs&) override;
  void OnStop(const StopArgs&) override;

 private:
  friend class perfetto::TrackEvent;

  static void WriteEvent(const char* category,
                         const char* name,
                         perfetto::protos::pbzero::TrackEvent::Type type) {
    Trace([category, name, type](TraceContext ctx) {
      WriteEventImpl(std::move(ctx), category, name, type);
    });
  }

  // Outlined to reduce binary size.
  static void WriteEventImpl(TraceContext ctx,
                             const char* category,
                             const char* name,
                             perfetto::protos::pbzero::TrackEvent::Type type);

  static void WriteSequenceDescriptors(
      internal::TrackEventDataSource::TraceContext*,
      uint64_t timestamp);
};

}  // namespace internal

}  // namespace perfetto

PERFETTO_DECLARE_DATA_SOURCE_STATIC_MEMBERS(
    perfetto::internal::TrackEventDataSource,
    perfetto::internal::TrackEventIncrementalState);

#endif  // INCLUDE_PERFETTO_TRACING_INTERNAL_TRACK_EVENT_DATA_SOURCE_H_
