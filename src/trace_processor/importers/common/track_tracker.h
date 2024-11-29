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
#include <tuple>
#include <variant>

#include "perfetto/base/compiler.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/hash.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/importers/common/tracks.h"
#include "src/trace_processor/importers/common/tracks_internal.h"
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

  explicit TrackTracker(TraceProcessorContext*);

  // Given a blueprint (i.e. the schema of a track), and the dimensions checks
  // whether the track has been seen before and if so, returns the id of the
  // seen track.
  //
  // If the track was *not* seen before, creates an entry in the track table
  // and returns the id.
  //
  // Usage (for slice tracks):
  //   ```
  //   void ParseMySpecialThreadScopedSlice(UniqueTid utid, ...(other args)) {
  //     static constexpr auto kBlueprint = tracks::SliceBlueprint(
  //       // The classification of the track.
  //       "my_special_thread_scoped_slice",
  //       // The dimensions of the track. Can be >1 if the track is broken down
  //       // by multiple fields.
  //       tracks::DimensionBlueprints(tracks::kThreadDimension)
  //     );
  //     TrackId track_id = track_tracker_->InternTrack(
  //         kBlueprint, tracks::Dimensions(utid));
  //
  //     ... add slices using SliceTracker
  //   }
  //   ```
  //
  // Usage (for counter tracks):
  //   ```
  //   void ParseMySpecialCustomScopedCounter(uint32_t custom_scope,
  //                                          ... other args) {
  //     static constexpr auto kBlueprint = tracks::CounterBlueprint(
  //       // The classification of the track.
  //       "my_special_custom_scoped_counter",
  //       // The dimensions of the track. Can be >1 if the track is broken down
  //       // by multiple fields.
  //       tracks::DimensionBlueprints(
  //           tracks::UnitDimensionBlueprint("custom_scope"))
  //     );
  //     TrackId track_id = track_tracker_->InternTrack(
  //         kBlueprint, tracks::Dimensions(custom_scope));
  //
  //     ... add counters using EventTracker
  //   }
  //   ```
  //
  // Note: when using this function, always try and check the blueprints in
  // `tracks_common.h` to see if there is a blueprint there which already does
  // what you need.
  template <typename Blueprint>
  PERFETTO_ALWAYS_INLINE TrackId
  InternTrack(const Blueprint& bp,
              typename Blueprint::dimensions_t dims = {},
              typename Blueprint::name_t name = tracks::BlueprintName(),
              const SetArgsCallback& args = {},
              typename Blueprint::unit_t unit = tracks::BlueprintUnit()) {
    base::Hasher hasher(bp.hasher);
    std::apply([&](auto&&... args) { ((hasher.Update(args)), ...); }, dims);
    auto [it, inserted] = tracks_new_.Insert(hasher.digest(), {});
    if (inserted) {
      std::array<GlobalArgsTracker::CompactArg, 8> a;
      DimensionsToArgs<0>(dims, bp.dimension_blueprints.data(), a.data());
      StringId n;
      using NBT = tracks::NameBlueprintT;
      using name_blueprint_t = typename Blueprint::name_blueprint_t;
      if constexpr (std::is_same_v<NBT::Auto, name_blueprint_t>) {
        n = kNullStringId;
      } else if constexpr (std::is_same_v<NBT::Static, name_blueprint_t>) {
        n = context_->storage->InternString(bp.name_blueprint.name);
      } else if constexpr (std::is_base_of_v<NBT::FnBase, name_blueprint_t>) {
        n = context_->storage->InternString(
            std::apply(bp.name_blueprint.fn, dims).string_view());
      } else {
        static_assert(std::is_same_v<NBT::Dynamic, name_blueprint_t>);
        n = name;
      }
      using UBT = tracks::UnitBlueprintT;
      using unit_blueprint_t = typename Blueprint::unit_blueprint_t;
      StringId u;
      if constexpr (std::is_same_v<UBT::Unknown, unit_blueprint_t>) {
        u = kNullStringId;
      } else if constexpr (std::is_same_v<UBT::Static, unit_blueprint_t>) {
        u = context_->storage->InternString(bp.unit_blueprint.name);
      } else {
        static_assert(std::is_same_v<UBT::Dynamic, unit_blueprint_t>);
        u = unit;
      }
      // GCC warns about the variables being unused even they are in certain
      // constexpr branches above. Just use them here to suppress the warning.
      base::ignore_result(name, unit);
      static constexpr uint32_t kDimensionCount =
          std::tuple_size_v<typename Blueprint::dimensions_t>;
      *it = AddTrack(bp, n, u, a.data(), kDimensionCount, args);
    }
    return *it;
  }

  // ********WARNING************
  // EVERYTHING BELOW THIS POINT IS LEGACY AND SHOULD BE REMOVED WITH TIME.
  // ********WARNING************

  // Dimensions of the data in a track. Used as an argument in
  // `TrackTracker::InternTrack()`. Use `TrackTracker::DimensionsBuilder` to
  // create.
  struct Dimensions {
    ArgSetId arg_set_id;

    bool operator==(const Dimensions& o) const {
      return arg_set_id == o.arg_set_id;
    }
  };

  // Used to create `Dimensions` required to intern a new track.
  class DimensionsBuilder {
   public:
    explicit DimensionsBuilder(TrackTracker* tt) : tt_(tt) {}

    // Append CPU dimension of a track.
    void AppendCpu(uint32_t cpu) {
      tt_->MarkCpuValid(cpu);
      AppendDimension(tt_->cpu_id_, Variadic::Integer(cpu));
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

  // Indicates that the track name will be automatically generated by the trace
  // processor.
  // All tracks *MUST* use this option unless it is explicitly approved by a
  // trace processor maintainer.
  struct AutoName {};
  // Indicates that the track name comes from the trace directly with no
  // modification.
  // *MUST NOT* be used without explicit appoval from a trace processor
  // maintainer.
  struct FromTraceName {
    StringId id;
  };
  // Indicates that the track name is synthesied in trace processor as a
  // StringId and works this way due to legacy reasons.
  //
  // Tracks *MUST NOT* use this method: this only exists for legacy tracks which
  // we named before the introduction of classification/dimension system.
  struct LegacyStringIdName {
    StringId id;
  };
  // Indicates that the track name is synthesied in trace processor as a
  // StackString and works this way due to legacy reasons.
  //
  // Tracks *MUST NOT* use this method: this only exists for legacy tracks which
  // we named before the introduction of classification/dimension system.
  struct LegacyCharArrayName {
    template <size_t N>
    explicit LegacyCharArrayName(const char (&_name)[N]) {
      static_assert(N > 0 && N <= 512);
      base::StringCopy(name, _name, N);
    }
    template <size_t N>
    explicit LegacyCharArrayName(const base::StackString<N>& _name) {
      static_assert(N > 0 && N <= 512);
      base::StringCopy(name, _name.c_str(), N);
    }
    char name[512];
  };
  using TrackName = std::
      variant<AutoName, FromTraceName, LegacyStringIdName, LegacyCharArrayName>;

  DimensionsBuilder CreateDimensionsBuilder() {
    return DimensionsBuilder(this);
  }

  // Interns track into TrackTable. If the track created with below arguments
  // already exists, returns the TrackTable::Id of the track.
  //
  // `name` is the display name of the track in trace processor and should
  // always be `AutoName()` unless approved by a trace processor maintainer.
  TrackId InternTrack(tracks::TrackClassification,
                      std::optional<Dimensions>,
                      const TrackName& name = AutoName(),
                      const SetArgsCallback& callback = {});

  // Interns a track with the given classification and one dimension into the
  // `track` table. This is useful when interning global tracks which have a
  // single uncommon dimension attached to them.
  TrackId InternSingleDimensionTrack(tracks::TrackClassification classification,
                                     StringId key,
                                     const Variadic& value,
                                     const TrackName& name = AutoName(),
                                     const SetArgsCallback& callback = {}) {
    return InternTrack(classification, SingleDimension(key, value), name,
                       callback);
  }

  // Interns counter track into TrackTable. If the track created with below
  // arguments already exists, returns the TrackTable::Id of the track.
  TrackId InternCounterTrack(tracks::TrackClassification,
                             std::optional<Dimensions>,
                             const TrackName& = AutoName());

  // Interns a unique track into the storage.
  TrackId InternGlobalTrack(tracks::TrackClassification,
                            const TrackName& = AutoName(),
                            const SetArgsCallback& callback = {});

  // Interns a thread track into the storage.
  TrackId InternThreadTrack(UniqueTid, const TrackName& = AutoName());

  // Interns a process track into the storage.
  TrackId InternProcessTrack(tracks::TrackClassification,
                             UniquePid,
                             const TrackName& = AutoName());

  // Interns a global track keyed by track type + CPU into the storage.
  TrackId InternCpuTrack(tracks::TrackClassification,
                         uint32_t cpu,
                         const TrackName& = AutoName());

  // Interns a counter track associated with a cpu into the storage.
  TrackId InternCpuCounterTrack(tracks::TrackClassification,
                                uint32_t cpu,
                                const TrackName& = AutoName());

  // Interns a counter track associated with a GPU into the storage.
  TrackId InternGpuCounterTrack(tracks::TrackClassification,
                                uint32_t gpu_id,
                                const TrackName& = AutoName());

  // Everything below this point are legacy functions and should no longer be
  // used.

  TrackId LegacyInternLegacyChromeAsyncTrack(StringId name,
                                             uint32_t upid,
                                             int64_t trace_id,
                                             bool trace_id_is_process_scoped,
                                             StringId source_scope);

  TrackId LegacyCreateGpuCounterTrack(StringId name,
                                      uint32_t gpu_id,
                                      StringId description = StringId::Null(),
                                      StringId unit = StringId::Null());

  TrackId LegacyCreatePerfCounterTrack(StringId name,
                                       tables::PerfSessionTable::Id,
                                       uint32_t cpu,
                                       bool is_timebase);

  TrackId LegacyInternThreadCounterTrack(StringId name, UniqueTid);

  TrackId LegacyInternGpuTrack(const tables::GpuTrackTable::Row&);

  TrackId LegacyInternProcessCounterTrack(StringId name,
                                          UniquePid,
                                          StringId unit = kNullStringId,
                                          StringId description = kNullStringId);

  TrackId LegacyInternGlobalCounterTrack(Group,
                                         StringId name,
                                         SetArgsCallback = {},
                                         StringId unit = kNullStringId,
                                         StringId description = kNullStringId);

 private:
  friend class AsyncTrackSetTracker;
  friend class TrackEventTracker;

  struct TrackMapKey {
    tracks::TrackClassification classification;
    std::optional<Dimensions> dimensions;

    bool operator==(const TrackMapKey& k) const {
      return std::tie(classification, dimensions) ==
             std::tie(k.classification, k.dimensions);
    }
  };

  struct MapHasher {
    size_t operator()(const TrackMapKey& l) const {
      perfetto::base::Hasher hasher;
      hasher.Update(static_cast<uint32_t>(l.classification));
      hasher.Update(l.dimensions ? l.dimensions->arg_set_id : -1ll);
      return hasher.digest();
    }
  };

  static constexpr size_t kGroupCount =
      static_cast<uint32_t>(Group::kSizeSentinel);

  TrackId CreateTrack(tracks::TrackClassification,
                      std::optional<Dimensions>,
                      const TrackName&);

  TrackId CreateCounterTrack(tracks::TrackClassification,
                             std::optional<Dimensions>,
                             const TrackName&);

  TrackId CreateThreadTrack(tracks::TrackClassification,
                            UniqueTid,
                            const TrackName&);

  TrackId CreateThreadCounterTrack(tracks::TrackClassification,
                                   UniqueTid,
                                   const TrackName&);

  TrackId CreateProcessTrack(tracks::TrackClassification,
                             UniquePid,
                             std::optional<Dimensions>,
                             const TrackName&);

  TrackId CreateProcessCounterTrack(tracks::TrackClassification,
                                    UniquePid,
                                    std::optional<Dimensions>,
                                    const TrackName&);

  TrackId InternTrackForGroup(Group);

  StringId StringIdFromTrackName(tracks::TrackClassification classification,
                                 const TrackTracker::TrackName& name);

  TrackId AddTrack(const tracks::BlueprintBase&,
                   StringId,
                   StringId,
                   GlobalArgsTracker::CompactArg*,
                   uint32_t,
                   const SetArgsCallback&);

  template <size_t i, typename TupleDimensions>
  void DimensionsToArgs(const TupleDimensions& dimensions,
                        const tracks::DimensionBlueprintBase* dimensions_schema,
                        GlobalArgsTracker::CompactArg* a) {
    static constexpr size_t kTupleSize = std::tuple_size_v<TupleDimensions>;
    if constexpr (i < kTupleSize) {
      using elem_t = std::tuple_element_t<i, TupleDimensions>;
      if constexpr (std::is_same_v<elem_t, uint32_t>) {
        if (dimensions_schema[i].is_cpu) {
          MarkCpuValid(std::get<i>(dimensions));
        }
        a[i].value = Variadic::Integer(std::get<i>(dimensions));
      } else if constexpr (std::is_integral_v<elem_t>) {
        a[i].value = Variadic::Integer(std::get<i>(dimensions));
      } else {
        static_assert(std::is_same_v<elem_t, base::StringView>,
                      "Unknown type for dimension");
        a[i].value = Variadic::String(
            context_->storage->InternString(std::get<i>(dimensions)));
      }
      DimensionsToArgs<i + 1>(dimensions, dimensions_schema, a);
    }
  }

  void MarkCpuValid(uint32_t cpu);

  Dimensions SingleDimension(StringId key, const Variadic& val) {
    std::array args{GlobalArgsTracker::CompactArg{key, key, val}};
    return Dimensions{
        context_->global_args_tracker->AddArgSet(args.data(), 0, 1)};
  }

  std::array<std::optional<TrackId>, kGroupCount> group_track_ids_;

  base::FlatHashMap<TrackMapKey, TrackId, MapHasher> tracks_;
  base::FlatHashMap<uint64_t, TrackId, base::AlreadyHashed<uint64_t>>
      tracks_new_;

  const StringId source_key_;
  const StringId trace_id_key_;
  const StringId trace_id_is_process_scoped_key_;
  const StringId source_scope_key_;
  const StringId category_key_;
  const StringId scope_id_;
  const StringId cookie_id_;

  const StringId fuchsia_source_;
  const StringId chrome_source_;

  const StringId utid_id_;
  const StringId upid_id_;
  const StringId cpu_id_;
  const StringId uid_id_;
  const StringId gpu_id_;
  const StringId name_id_;

  TraceProcessorContext* const context_;
  ArgsTracker args_tracker_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_TRACK_TRACKER_H_
