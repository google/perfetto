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

#ifndef SRC_TRACE_PROCESSOR_TRACK_TRACKER_H_
#define SRC_TRACE_PROCESSOR_TRACK_TRACKER_H_

#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

// Tracks and stores tracks based on track types, ids and scopes.
class TrackTracker {
 public:
  explicit TrackTracker(TraceProcessorContext*);

  // Interns a given Fuchsia async track into the storage.
  TrackId InternFuchsiaAsyncTrack(
      const tables::FuchsiaAsyncTrackTable::Row& row);

  // Interns a given GPU track into the storage.
  TrackId InternGpuTrack(const tables::GpuTrackTable::Row& row);

  // Interns a given Chrome async track into the storage.
  TrackId InternChromeAsyncTrack(const tables::ChromeAsyncTrackTable::Row& row,
                                 StringId source_scope);

 private:
  struct FuchsiaAsyncTrackTuple {
    int64_t correlation_id;

    friend bool operator<(const FuchsiaAsyncTrackTuple& l,
                          const FuchsiaAsyncTrackTuple& r) {
      return std::tie(l.correlation_id) < std::tie(r.correlation_id);
    }
  };
  struct GpuTrackTuple {
    StringId track_name;
    StringId scope;

    friend bool operator<(const GpuTrackTuple& l, const GpuTrackTuple& r) {
      return std::tie(l.track_name, l.scope) < std::tie(r.track_name, r.scope);
    }
  };
  struct ChromeTrackTuple {
    enum class Scope : uint32_t {
      kGlobal = 0,
      kProcess,
    };

    Scope scope;
    base::Optional<int64_t> upid;
    int64_t source_id = 0;
    StringId source_scope = 0;

    friend bool operator<(const ChromeTrackTuple& l,
                          const ChromeTrackTuple& r) {
      return std::tie(l.source_id, l.scope, l.upid, l.source_scope) <
             std::tie(r.source_id, r.scope, r.upid, r.source_scope);
    }
  };

  std::map<FuchsiaAsyncTrackTuple, TrackId> fuchsia_async_tracks_;
  std::map<GpuTrackTuple, TrackId> gpu_tracks_;
  std::map<ChromeTrackTuple, TrackId> chrome_tracks_;
  TraceProcessorContext* const context_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TRACK_TRACKER_H_
