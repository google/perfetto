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

  // Interns a process track into the storage.
  TrackId InternProcessTrack(UniquePid upid);

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

  // Associate a TrackDescriptor track identified by the given |uuid| with a
  // process's |pid|. This is called during tokenization. If a reservation for
  // the same |uuid| already exists, verifies that the present reservation
  // matches the new one.
  //
  // The track will be resolved to the process track (see InternProcessTrack())
  // upon the first call to GetDescriptorTrack() with the same |uuid|. At this
  // time, |pid| will also be resolved to a |upid|.
  void ReserveDescriptorProcessTrack(uint64_t uuid,
                                     uint32_t pid,
                                     int64_t timestamp);

  // Associate a TrackDescriptor track identified by the given |uuid| with a
  // thread's |pid| and |tid|. This is called during tokenization. If a
  // reservation for the same |uuid| already exists, verifies that the present
  // reservation matches the new one.
  //
  // The track will be resolved to the thread track (see InternThreadTrack())
  // upon the first call to GetDescriptorTrack() with the same |uuid|. At this
  // time, |pid| will also be resolved to a |upid|.
  void ReserveDescriptorThreadTrack(uint64_t uuid,
                                    uint64_t parent_uuid,
                                    uint32_t pid,
                                    uint32_t tid,
                                    int64_t timestamp);

  // Associate a TrackDescriptor track identified by the given |uuid| with a
  // parent track (usually a process- or thread-associated track). This is
  // called during tokenization. If a reservation for the same |uuid| already
  // exists, will attempt to update it.
  //
  // The track will be created upon the first call to GetDescriptorTrack() with
  // the same |uuid|. If |parent_uuid| is 0, the track will become a global
  // track. Otherwise, it will become a new track of the same type as its parent
  // track.
  void ReserveDescriptorChildTrack(uint64_t uuid, uint64_t parent_uuid);

  // Returns the ID of the track for the TrackDescriptor with the given |uuid|.
  // This is called during parsing. The first call to GetDescriptorTrack() for
  // each |uuid| resolves and inserts the track (and its parent tracks,
  // following the parent_uuid chain recursively) based on reservations made for
  // the |uuid|. Returns nullopt if no track for a descriptor with this |uuid|
  // has been reserved.
  base::Optional<TrackId> GetDescriptorTrack(uint64_t uuid);

  // Returns the ID of the implicit trace-global default TrackDescriptor track.
  TrackId GetOrCreateDefaultDescriptorTrack();

  // Interns a global counter track into the storage.
  TrackId InternGlobalCounterTrack(StringId name);

  // Interns a counter track associated with a cpu into the storage.
  TrackId InternCpuCounterTrack(StringId name, uint32_t cpu);

  // Interns a counter track associated with a thread into the storage.
  TrackId InternThreadCounterTrack(StringId name, UniqueTid utid);

  // Interns a counter track associated with a process into the storage.
  TrackId InternProcessCounterTrack(StringId name, UniquePid upid);

  // Interns a counter track associated with an irq into the storage.
  TrackId InternIrqCounterTrack(StringId name, int32_t irq);

  // Interns a counter track associated with an softirq into the storage.
  TrackId InternSoftirqCounterTrack(StringId name, int32_t softirq);

  // Interns a counter track associated with a GPU into the storage.
  TrackId InternGpuCounterTrack(StringId name, uint32_t gpu_id);

  // Creates a counter track associated with a GPU into the storage.
  TrackId CreateGpuCounterTrack(StringId name,
                                uint32_t gpu_id,
                                StringId description = StringId::Null(),
                                StringId unit = StringId::Null());

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
    StringId source_scope = StringId::Null();

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
  struct DescriptorTrackReservation {
    uint64_t parent_uuid = 0;
    base::Optional<uint32_t> pid;
    base::Optional<uint32_t> tid;
    int64_t min_timestamp = 0;  // only set if |pid| and/or |tid| is set.

    // Whether |other| is a valid descriptor for this track reservation. A track
    // should always remain nested underneath its original parent.
    bool IsForSameTrack(const DescriptorTrackReservation& other) {
      // Note that |timestamp| is ignored for this comparison.
      return std::tie(parent_uuid, pid, tid) ==
             std::tie(other.parent_uuid, pid, tid);
    }
  };

  TrackId ResolveDescriptorTrack(uint64_t uuid,
                                 const DescriptorTrackReservation&);

  static constexpr uint64_t kDefaultDescriptorTrackUuid = 0u;

  std::map<UniqueTid, TrackId> thread_tracks_;
  std::map<UniquePid, TrackId> process_tracks_;
  std::map<int64_t /* correlation_id */, TrackId> fuchsia_async_tracks_;
  std::map<GpuTrackTuple, TrackId> gpu_tracks_;
  std::map<ChromeTrackTuple, TrackId> chrome_tracks_;
  std::map<AndroidAsyncTrackTuple, TrackId> android_async_tracks_;
  std::map<UniquePid, TrackId> chrome_process_instant_tracks_;
  base::Optional<TrackId> chrome_global_instant_track_id_;
  std::map<uint64_t /* uuid */, DescriptorTrackReservation>
      reserved_descriptor_tracks_;
  std::map<uint64_t /* uuid */, TrackId> resolved_descriptor_tracks_;

  std::map<StringId, TrackId> global_counter_tracks_by_name_;
  std::map<std::pair<StringId, uint32_t>, TrackId> cpu_counter_tracks_;
  std::map<std::pair<StringId, UniqueTid>, TrackId> utid_counter_tracks_;
  std::map<std::pair<StringId, UniquePid>, TrackId> upid_counter_tracks_;
  std::map<std::pair<StringId, int32_t>, TrackId> irq_counter_tracks_;
  std::map<std::pair<StringId, int32_t>, TrackId> softirq_counter_tracks_;
  std::map<std::pair<StringId, uint32_t>, TrackId> gpu_counter_tracks_;

  // Stores the descriptor uuid used for the primary process/thread track
  // for the given upid / utid. Used for pid/tid reuse detection.
  std::map<UniquePid, uint64_t /*uuid*/> descriptor_uuids_by_upid_;
  std::map<UniqueTid, uint64_t /*uuid*/> descriptor_uuids_by_utid_;

  const StringId source_key_ = kNullStringId;
  const StringId source_id_key_ = kNullStringId;
  const StringId source_id_is_process_scoped_key_ = kNullStringId;
  const StringId source_scope_key_ = kNullStringId;
  const StringId parent_track_id_key_ = kNullStringId;

  const StringId fuchsia_source_ = kNullStringId;
  const StringId chrome_source_ = kNullStringId;
  const StringId android_source_ = kNullStringId;
  const StringId descriptor_source_ = kNullStringId;

  const StringId default_descriptor_track_name_ = kNullStringId;

  TraceProcessorContext* const context_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TRACK_TRACKER_H_
