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

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/hash.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/cpu_tracker.h"
#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/importers/common/track_classification.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/tables/track_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"

namespace perfetto::trace_processor {

// Tracks and stores tracks based on track types, ids and scopes.
class TrackTracker {
 public:
  using SetArgsCallback = std::function<void(ArgsTracker::BoundInserter&)>;

  // Dimensions of the data in a track. Used as an argument in
  // `TrackTracker::InternTrack()`. Use `TrackTracker::DimensionsBuilder` to
  // create.
  struct Dimensions {
    ArgSetId arg_set_id;
  };

  // Used to create `Dimensions` required to intern a new track.
  class DimensionsBuilder {
   public:
    explicit DimensionsBuilder(TrackTracker* tt) : tt_(tt) {}

    // Append CPU dimension of a track.
    void AppendCpu(int64_t cpu_id) {
      AppendDimension(tt_->ucpu_id_, Variadic::Integer(cpu_id));
    }

    // Append Utid (unique tid) dimension of a track.
    void AppendUtid(UniqueTid utid) {
      AppendDimension(tt_->utid_id_, Variadic::Integer(utid));
    }

    // Append Upid (unique pid) dimension of a track.
    void AppendUpid(UniquePid upid) {
      AppendDimension(tt_->upid_id_, Variadic::Integer(upid));
    }

    // Append gpu id dimension of a track.
    void AppendGpu(int64_t gpu) {
      AppendDimension(tt_->gpu_id_, Variadic::Integer(gpu));
    }

    // Append Uid (user id) dimension of a track.
    void AppendUid(int64_t uid) {
      AppendDimension(tt_->uid_id_, Variadic::Integer(uid));
    }

    // Append name dimension of a track. Only use in cases where name is a
    // dimension, it is not a way to force the name of the track in a table.
    void AppendName(StringId name) {
      AppendDimension(tt_->name_id_, Variadic::String(name));
    }

    // Append custom dimension. Only use if none of the other Append functions
    // are suitable.
    void AppendDimension(StringId key, const Variadic& val) {
      GlobalArgsTracker::CompactArg& arg = args_[count_args++];
      arg.flat_key = key;
      arg.key = key;
      arg.value = val;
    }

    // Build to fetch the `Dimensions` value of the Appended dimensions. Pushes
    // the dimensions into args table. Use the result in
    // `TrackTracker::InternTrack`.
    Dimensions Build() && {
      return Dimensions{tt_->context_->global_args_tracker->AddArgSet(
          args_.data(), 0, count_args)};
    }

   private:
    TrackTracker* tt_;
    std::array<GlobalArgsTracker::CompactArg, 64> args_;
    uint32_t count_args = 0;
  };

  // Enum which groups global tracks to avoid an explosion of tracks at the top
  // level.
  // Try and keep members of this enum high level as every entry here
  // corresponds to ~1 extra UI track.
  enum class Group : uint32_t {
    kMemory = 0,
    kIo,
    kVirtio,
    kNetwork,
    kPower,
    kDeviceState,
    kThermals,
    kClockFrequency,
    kBatteryMitigation,

    // Keep this last.
    kSizeSentinel,
  };

  explicit TrackTracker(TraceProcessorContext*);

  DimensionsBuilder CreateDimensionsBuilder() {
    return DimensionsBuilder(this);
  }

  // Interns track into TrackTable. If the track created with below arguments
  // already exists, returns the TrackTable::Id of the track.
  TrackId InternTrack(TrackClassification,
                      std::optional<Dimensions>,
                      StringId name,
                      const SetArgsCallback& callback = {});

  // Interns a track with the given classification and one dimension into the
  // `track` table. This is useful when interning global tracks which have a
  // single uncommon dimension attached to them.
  //
  // Note: name is *not* used relevant for interning: it's used purely as a
  // display name.
  TrackId InternSingleDimensionTrack(TrackClassification classification,
                                     StringId key,
                                     const Variadic& value,
                                     StringId name,
                                     const SetArgsCallback& callback = {}) {
    return InternTrack(classification, SingleDimension(key, value), name,
                       callback);
  }

  // Interns counter track into TrackTable. If the track created with below
  // arguments already exists, returns the TrackTable::Id of the track.
  TrackId InternCounterTrack(TrackClassification,
                             std::optional<Dimensions>,
                             StringId name);

  // Interns a unique track into the storage.
  TrackId InternGlobalTrack(TrackClassification);

  // Interns a global counter track into the storage.
  // TODO(mayzner): Cleanup the arguments to be consistent with other Intern
  // functions.
  TrackId InternGlobalCounterTrack(Group,
                                   StringId name,
                                   SetArgsCallback = {},
                                   StringId unit = kNullStringId,
                                   StringId description = kNullStringId);

  // Interns a thread track into the storage.
  TrackId InternThreadTrack(UniqueTid);

  // Interns a counter track associated with a thread into the storage.
  // TODO(mayzner): Cleanup the arguments to be consistent with other Intern
  // functions.
  TrackId InternThreadCounterTrack(StringId name, UniqueTid);

  TrackId InternProcessTrack(TrackClassification,
                             UniquePid,
                             StringId name = kNullStringId);

  // Interns a process track into the storage.
  TrackId InternProcessTrack(UniquePid);

  TrackId InternProcessCounterTrack(UniquePid);

  // Interns a counter track associated with a process into the storage.
  // TODO(mayzner): Cleanup the arguments to be consistent with other Intern
  // functions.
  TrackId InternProcessCounterTrack(StringId name,
                                    UniquePid,
                                    StringId unit = kNullStringId,
                                    StringId description = kNullStringId);

  // Interns a global track keyed by track type + CPU into the storage.
  TrackId InternCpuTrack(TrackClassification, uint32_t cpu);

  // Interns a counter track associated with a cpu into the storage.
  TrackId InternCpuCounterTrack(TrackClassification, uint32_t cpu);

  // Interns a given GPU track into the storage.
  // TODO(mayzner): Cleanup the arguments to be consistent with other Intern
  // functions.
  TrackId InternGpuTrack(const tables::GpuTrackTable::Row&);

  // Interns a counter track associated with a GPU into the storage.
  TrackId InternGpuCounterTrack(TrackClassification, uint32_t gpu_id);

  // Interns a GPU work period track into the storage.
  // TODO(mayzner): Remove when all usages migrated to new track design.
  TrackId LegacyInternGpuWorkPeriodTrack(
      const tables::GpuWorkPeriodTrackTable::Row& row);

  // Interns a legacy Chrome async event track into the storage.
  // TODO(mayzner): Remove when all usages migrated to new track design.
  TrackId LegacyInternLegacyChromeAsyncTrack(StringId name,
                                             uint32_t upid,
                                             int64_t trace_id,
                                             bool trace_id_is_process_scoped,
                                             StringId source_scope);

  // Interns a counter track associated with a cpu into the storage.
  // TODO(mayzner): Remove when all usages migrated to new track design.
  TrackId LegacyInternCpuIdleStateTrack(uint32_t cpu, StringId state);

  // Interns a counter track associated with an irq into the storage.
  // TODO(mayzner): Remove when all usages migrated to new track design.
  TrackId LegacyInternIrqCounterTrack(TrackClassification, int32_t irq);

  // Interns a counter track associated with an softirq into the storage.
  // TODO(mayzner): Remove when all usages migrated to new track design.
  TrackId LegacyInternSoftirqCounterTrack(TrackClassification, int32_t softirq);

  // Creates a counter track associated with a GPU into the storage.
  // TODO(mayzner): Remove when all usages migrated to new track design.
  TrackId LegacyCreateGpuCounterTrack(StringId name,
                                      uint32_t gpu_id,
                                      StringId description = StringId::Null(),
                                      StringId unit = StringId::Null());

  // Creates a counter track for values within perf samples.
  // The tracks themselves are managed by PerfSampleTracker.
  // TODO(mayzner): Remove when all usages migrated to new track design.
  TrackId LegacyCreatePerfCounterTrack(StringId name,
                                       tables::PerfSessionTable::Id,
                                       uint32_t cpu,
                                       bool is_timebase);

  // Interns a Fuchsia async track into the storage.
  // TODO(mayzner): Remove when all usages migrated to new track design.
  TrackId LegacyInternFuchsiaAsyncTrack(StringId name,
                                        uint32_t upid,
                                        int64_t correlation_id);

 private:
  friend class AsyncTrackSetTracker;
  friend class TrackEventTracker;

  struct TrackMapKey {
    TrackClassification classification;
    std::optional<Dimensions> dimensions;
    std::optional<StringId> name = std::nullopt;

    // TODO(mayzner): Remove after cleaning Chrome legacy tracks.
    std::optional<int64_t> cookie = std::nullopt;

    bool operator==(const TrackMapKey& k) const {
      if (classification != k.classification)
        return false;
      if ((dimensions.has_value() && !k.dimensions.has_value()) ||
          (k.dimensions.has_value() && !dimensions.has_value()))
        return false;
      if (dimensions.has_value() &&
          (dimensions->arg_set_id != k.dimensions->arg_set_id))
        return false;
      if (name != k.name)
        return false;
      if (cookie != k.cookie)
        return false;
      return true;
    }
  };

  struct MapHasher {
    size_t operator()(const TrackMapKey& l) const {
      perfetto::base::Hasher hasher;
      hasher.Update(static_cast<uint32_t>(l.classification));
      hasher.Update(l.dimensions.has_value());
      if (l.dimensions.has_value()) {
        hasher.Update(l.dimensions->arg_set_id);
      }
      hasher.Update(l.name.value_or(kNullStringId).raw_id());
      hasher.Update(l.cookie.value_or(-1));
      return hasher.digest();
    }
  };

  static constexpr size_t kGroupCount =
      static_cast<uint32_t>(Group::kSizeSentinel);

  TrackId CreateTrack(TrackClassification,
                      std::optional<Dimensions>,
                      StringId name);

  TrackId CreateCounterTrack(TrackClassification,
                             std::optional<Dimensions>,
                             StringId name);

  TrackId CreateThreadTrack(TrackClassification, UniqueTid);

  TrackId CreateThreadCounterTrack(TrackClassification,
                                   StringId name,
                                   UniqueTid);

  TrackId CreateProcessTrack(TrackClassification,
                             UniquePid,
                             std::optional<Dimensions> = std::nullopt,
                             StringId name = kNullStringId);

  TrackId CreateProcessCounterTrack(TrackClassification,
                                    UniquePid,
                                    std::optional<Dimensions> = std::nullopt);

  TrackId InternTrackForGroup(Group);

  Dimensions SingleDimension(StringId key, const Variadic& val) {
    std::array args{GlobalArgsTracker::CompactArg{key, key, val}};
    return Dimensions{
        context_->global_args_tracker->AddArgSet(args.data(), 0, 1)};
  }

  std::array<std::optional<TrackId>, kGroupCount> group_track_ids_;

  base::FlatHashMap<TrackMapKey, TrackId, MapHasher> tracks_;

  const StringId source_key_ = kNullStringId;
  const StringId trace_id_key_ = kNullStringId;
  const StringId trace_id_is_process_scoped_key_ = kNullStringId;
  const StringId source_scope_key_ = kNullStringId;
  const StringId category_key_ = kNullStringId;

  const StringId fuchsia_source_ = kNullStringId;
  const StringId chrome_source_ = kNullStringId;

  const StringId utid_id_;
  const StringId upid_id_;
  const StringId ucpu_id_;
  const StringId uid_id_;
  const StringId gpu_id_;
  const StringId name_id_;

  TraceProcessorContext* const context_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_TRACK_TRACKER_H_
