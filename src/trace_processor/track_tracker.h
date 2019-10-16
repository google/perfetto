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

  // Interns a thread track into the storage.
  TrackId InternThreadTrack(UniqueTid utid);

  // Interns a Fuchsia async track into the storage.
  TrackId InternFuchsiaAsyncTrack(StringId name, int64_t correlation_id);

  // Interns a given GPU track into the storage.
  TrackId InternGpuTrack(const tables::GpuTrackTable::Row& row);

  // Interns a legacy Chrome async event track into the storage.
  TrackId InternLegacyChromeAsyncTrack(StringId name,
                                       uint32_t upid,
                                       int64_t source_id,
                                       bool source_id_is_process_scoped,
                                       StringId source_scope);

  // Interns a Android async track into the storage.
  TrackId InternAndroidAsyncTrack(StringId name,
                                  UniquePid upid,
                                  int64_t cookie);

  // Interns a track for legacy Chrome process-scoped instant events into the
  // storage.
  TrackId InternLegacyChromeProcessInstantTrack(UniquePid upid);

  // Lazily creates the track for legacy Chrome global instant events.
  TrackId GetOrCreateLegacyChromeGlobalInstantTrack();

  // Create or update the track for the TrackDescriptor with the given |uuid|.
  // Optionally, associate the track with a process or thread.
  TrackId UpdateDescriptorTrack(uint64_t uuid,
                                StringId name,
                                base::Optional<UniquePid> upid = base::nullopt,
                                base::Optional<UniqueTid> utid = base::nullopt);

  // Returns the ID of the track for the TrackDescriptor with the given |uuid|.
  // Returns nullopt if no TrackDescriptor with this |uuid| has been parsed yet.
  base::Optional<TrackId> GetDescriptorTrack(uint64_t uuid) const;

  // Returns the ID of the TrackDescriptor track associated with the given utid.
  // If the trace contained multiple tracks associated with the utid, the first
  // created track is returned. Creates a new track if no such track exists.
  TrackId GetOrCreateDescriptorTrackForThread(UniqueTid utid);

  // Returns the ID of the implicit trace-global default TrackDescriptor track.
  TrackId GetOrCreateDefaultDescriptorTrack();

 private:
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

  static constexpr TrackId kDefaultDescriptorTrackUuid = 0u;

  std::map<UniqueTid /* utid */, TrackId> thread_tracks_;
  std::map<int64_t /* correlation_id */, TrackId> fuchsia_async_tracks_;
  std::map<GpuTrackTuple, TrackId> gpu_tracks_;
  std::map<ChromeTrackTuple, TrackId> chrome_tracks_;
  std::map<AndroidAsyncTrackTuple, TrackId> android_async_tracks_;
  std::map<UniquePid, TrackId> chrome_process_instant_tracks_;
  base::Optional<TrackId> chrome_global_instant_track_id_;
  std::map<uint64_t /* uuid */, TrackId> descriptor_tracks_;
  std::map<UniqueTid, TrackId> descriptor_tracks_by_utid_;

  const StringId source_key_ = 0;
  const StringId source_id_key_ = 0;
  const StringId source_scope_key_ = 0;

  const StringId fuchsia_source_ = 0;
  const StringId chrome_source_ = 0;
  const StringId android_source_ = 0;
  const StringId descriptor_source_ = 0;

  const StringId default_descriptor_track_name_ = 0;

  TraceProcessorContext* const context_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TRACK_TRACKER_H_
