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

  // Interns a Fuchsia async track into the storage.
  TrackId InternFuchsiaAsyncTrack(StringId name, int64_t correlation_id);

  // Interns a given GPU track into the storage.
  TrackId InternGpuTrack(const tables::GpuTrackTable::Row& row);

  // Interns a Chrome track into the storage.
  TrackId InternChromeTrack(StringId name,
                            base::Optional<uint32_t> upid,
                            int64_t source_id,
                            StringId source_scope);

  // Interns a Android async track into the storage.
  TrackId InternAndroidAsyncTrack(StringId name, uint32_t upid, int64_t cookie);

 private:
  struct FuchsiaAsyncTrackTuple {
    int64_t correlation_id;

    friend bool operator<(const FuchsiaAsyncTrackTuple& l,
                          const FuchsiaAsyncTrackTuple& r) {
      return l.correlation_id < r.correlation_id;
    }
  };
  struct GpuTrackTuple {
    StringId track_name;
    StringId scope;
    int64_t context_id;

    friend bool operator<(const GpuTrackTuple& l, const GpuTrackTuple& r) {
      return std::tie(l.track_name, l.scope, l.context_id)
          < std::tie(r.track_name, r.scope, r.context_id);
    }
  };
  struct ChromeTrackTuple {
    base::Optional<int64_t> upid;
    int64_t source_id = 0;
    StringId source_scope = 0;

    friend bool operator<(const ChromeTrackTuple& l,
                          const ChromeTrackTuple& r) {
      return std::tie(l.source_id, l.upid, l.source_scope) <
             std::tie(r.source_id, r.upid, r.source_scope);
    }
  };
  struct AndroidAsyncTrackTuple {
    UniquePid upid;
    int64_t cookie;
    StringId name;

    friend bool operator<(const AndroidAsyncTrackTuple& l,
                          const AndroidAsyncTrackTuple& r) {
      return std::tie(l.upid, l.cookie, l.name) <
             std::tie(r.upid, r.cookie, r.name);
    }
  };

  std::map<FuchsiaAsyncTrackTuple, TrackId> fuchsia_async_tracks_;
  std::map<GpuTrackTuple, TrackId> gpu_tracks_;
  std::map<ChromeTrackTuple, TrackId> chrome_tracks_;
  std::map<AndroidAsyncTrackTuple, TrackId> android_async_tracks_;

  StringId source_key_ = 0;
  StringId source_id_key_ = 0;
  StringId source_scope_key_ = 0;

  StringId fuchsia_source_ = 0;
  StringId chrome_source_ = 0;
  StringId android_source_ = 0;

  TraceProcessorContext* const context_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TRACK_TRACKER_H_
