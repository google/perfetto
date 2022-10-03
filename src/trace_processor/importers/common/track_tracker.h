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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_TRACK_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_TRACK_TRACKER_H_

#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

// Tracks and stores tracks based on track types, ids and scopes.
class TrackTracker {
 public:
  using SetArgsCallback = std::function<void(ArgsTracker::BoundInserter&)>;

  explicit TrackTracker(TraceProcessorContext*);

  // Interns a thread track into the storage.
  TrackId InternThreadTrack(UniqueTid utid);

  // Interns a process track into the storage.
  TrackId InternProcessTrack(UniquePid upid);

  // Interns a Fuchsia async track into the storage.
  TrackId InternFuchsiaAsyncTrack(StringId name,
                                  uint32_t upid,
                                  int64_t correlation_id);

  // Interns a global track keyed by CPU + name into the storage.
  TrackId InternCpuTrack(StringId name, uint32_t cpu);

  // Interns a given GPU track into the storage.
  TrackId InternGpuTrack(const tables::GpuTrackTable::Row& row);

  // Interns a legacy Chrome async event track into the storage.
  TrackId InternLegacyChromeAsyncTrack(StringId name,
                                       uint32_t upid,
                                       int64_t source_id,
                                       bool source_id_is_process_scoped,
                                       StringId source_scope);

  // Interns a track for legacy Chrome process-scoped instant events into the
  // storage.
  TrackId InternLegacyChromeProcessInstantTrack(UniquePid upid);

  // Lazily creates the track for legacy Chrome global instant events.
  TrackId GetOrCreateLegacyChromeGlobalInstantTrack();

  // Returns the ID of the implicit trace-global default track for triggers
  // received by the service.
  TrackId GetOrCreateTriggerTrack();

  // Interns a global counter track into the storage.
  TrackId InternGlobalCounterTrack(StringId name,
                                   SetArgsCallback = {},
                                   StringId unit = kNullStringId,
                                   StringId description = kNullStringId);

  // Interns a counter track associated with a cpu into the storage.
  TrackId InternCpuCounterTrack(StringId name, uint32_t cpu);

  // Interns a counter track associated with a thread into the storage.
  TrackId InternThreadCounterTrack(StringId name, UniqueTid utid);

  // Interns a counter track associated with a process into the storage.
  TrackId InternProcessCounterTrack(StringId name,
                                    UniquePid upid,
                                    StringId unit = kNullStringId,
                                    StringId description = kNullStringId);

  // Interns a counter track associated with an irq into the storage.
  TrackId InternIrqCounterTrack(StringId name, int32_t irq);

  // Interns a counter track associated with an softirq into the storage.
  TrackId InternSoftirqCounterTrack(StringId name, int32_t softirq);

  // Interns a counter track associated with a GPU into the storage.
  TrackId InternGpuCounterTrack(StringId name, uint32_t gpu_id);

  // Interns energy counter track associated with a
  // Energy breakdown into the storage.
  TrackId InternEnergyCounterTrack(StringId name,
                                   int32_t consumer_id,
                                   StringId consumer_type,
                                   int32_t ordinal);
  // Interns a per process energy counter track associated with a
  // Energy into the storage.
  TrackId InternUidCounterTrack(StringId name, int32_t uid);

  // Interns a per process energy consumer counter track associated with a
  // Energy Uid into the storage.
  TrackId InternEnergyPerUidCounterTrack(StringId name,
                                         int32_t consumer_id,
                                         int32_t uid);

  // Creates a counter track associated with a GPU into the storage.
  TrackId CreateGpuCounterTrack(StringId name,
                                uint32_t gpu_id,
                                StringId description = StringId::Null(),
                                StringId unit = StringId::Null());

  // Creates a counter track for values within perf samples.
  // The tracks themselves are managed by PerfSampleTracker.
  TrackId CreatePerfCounterTrack(StringId name,
                                 uint32_t perf_session_id,
                                 uint32_t cpu,
                                 bool is_timebase);

  // NOTE:
  // The below method should only be called by AsyncTrackSetTracker

  // Creates and inserts a global async track into the storage.
  TrackId CreateGlobalAsyncTrack(StringId name, StringId source);

  // Creates and inserts a Android async track into the storage.
  TrackId CreateProcessAsyncTrack(StringId name,
                                  UniquePid upid,
                                  StringId source);

 private:
  struct GpuTrackTuple {
    StringId track_name;
    StringId scope;
    int64_t context_id;

    friend bool operator<(const GpuTrackTuple& l, const GpuTrackTuple& r) {
      return std::tie(l.track_name, l.scope, l.context_id) <
             std::tie(r.track_name, r.scope, r.context_id);
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

  std::map<UniqueTid, TrackId> thread_tracks_;
  std::map<UniquePid, TrackId> process_tracks_;
  std::map<int64_t /* correlation_id */, TrackId> fuchsia_async_tracks_;

  std::map<std::pair<StringId, uint32_t /* cpu */>, TrackId> cpu_tracks_;

  std::map<GpuTrackTuple, TrackId> gpu_tracks_;
  std::map<ChromeTrackTuple, TrackId> chrome_tracks_;
  std::map<UniquePid, TrackId> chrome_process_instant_tracks_;

  std::map<StringId, TrackId> global_counter_tracks_by_name_;
  std::map<std::pair<StringId, uint32_t>, TrackId> cpu_counter_tracks_;
  std::map<std::pair<StringId, UniqueTid>, TrackId> utid_counter_tracks_;
  std::map<std::pair<StringId, UniquePid>, TrackId> upid_counter_tracks_;
  std::map<std::pair<StringId, int32_t>, TrackId> irq_counter_tracks_;
  std::map<std::pair<StringId, int32_t>, TrackId> softirq_counter_tracks_;
  std::map<std::pair<StringId, uint32_t>, TrackId> gpu_counter_tracks_;
  std::map<std::pair<StringId, int32_t>, TrackId> energy_counter_tracks_;
  std::map<std::pair<StringId, int32_t>, TrackId> uid_counter_tracks_;
  std::map<std::pair<StringId, int32_t>, TrackId>
      energy_per_uid_counter_tracks_;

  base::Optional<TrackId> chrome_global_instant_track_id_;
  base::Optional<TrackId> trigger_track_id_;

  const StringId source_key_ = kNullStringId;
  const StringId source_id_key_ = kNullStringId;
  const StringId source_id_is_process_scoped_key_ = kNullStringId;
  const StringId source_scope_key_ = kNullStringId;
  const StringId category_key_ = kNullStringId;

  const StringId fuchsia_source_ = kNullStringId;
  const StringId chrome_source_ = kNullStringId;

  TraceProcessorContext* const context_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_TRACK_TRACKER_H_
