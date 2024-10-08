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
#include <optional>
#include <string>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/cpu_tracker.h"
#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"

namespace perfetto {
namespace trace_processor {

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
    void AppendDimension(StringId key, Variadic val) {
      GlobalArgsTracker::Arg arg;
      arg.flat_key = key;
      arg.key = key;
      arg.value = val;

      // Those will not be used.
      arg.row = std::numeric_limits<uint32_t>::max();
      arg.column = nullptr;

      args_[count_args] = std::move(arg);
      count_args++;
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
    std::array<GlobalArgsTracker::Arg, 64> args_;
    uint32_t count_args = 0;
  };

  // The classification of the track. Each track has to have one of those.
  // Together with dimensions they are responsible for uniquely identifying the
  // track.
  // When adding new track prefer to choose one of the below if possible, with
  // remembering that dimensions are also used to identify the track.
  //
  // Example: under classification kAsync, you can have both global async tracks
  // and process async tracks. The difference between those is in the fact that
  // process async tracks will also have "upid" dimension.
  enum class TrackClassification {
    // Global tracks, unique per trace.
    kTrigger,
    kInterconnect,
    kChromeLegacyGlobalInstant,

    // General tracks.
    kThread,
    kProcess,
    kLinuxDevice,
    kChromeProcessInstant,
    kAsync,

    // Irq tracks.
    kIrqCount,

    // Softirq tracks.
    kSoftirqCount,

    // Gpu tracks.
    kGpuFrequency,

    // Cpu tracks.
    kIrqCpu,
    kSoftirqCpu,
    kNapiGroCpu,
    kMaliIrqCpu,
    kMinFreqCpu,
    kMaxFreqCpu,
    kFuncgraphCpu,
    kPkvmHypervisor,

    // Cpu counter tracks.
    kCpuFrequency,
    kCpuFrequencyThrottle,
    kCpuMaxFrequencyLimit,
    kCpuMinFrequencyLimit,

    kCpuIdle,
    kCpuIdleState,
    kCpuUtilization,
    kCpuCapacity,
    kCpuNumberRunning,

    // Time CPU spent in state.
    kUserTime,
    kNiceUserTime,
    kSystemModeTime,
    kIoWaitTime,
    kIrqTime,
    kSoftIrqTime,
    kCpuIdleTime,

    // Not set. Legacy, never use for new tracks.
    // If set the classification can't be used to decide the tracks and
    // dimensions + name should be used instead. Strongly discouraged.
    kUnknown
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
                      std::optional<Dimensions> dimensions,
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

  // Interns a process track into the storage.
  TrackId InternProcessTrack(UniquePid);

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

  // Pushes async track into TrackTable. Returns the TrackTable::Id of the newly
  // created track.
  // TODO(mayzner): make name optional and later remove the option of adding it
  // fully. Those changes can be made when we add support for autogenerating
  // names.
  TrackId CreateAsyncTrack(std::optional<Dimensions> dimensions, StringId name);

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

  // Interns a track for legacy Chrome process-scoped instant events into the
  // storage.
  // TODO(mayzner): Remove when all usages migrated to new track design.
  TrackId InternLegacyChromeProcessInstantTrack(UniquePid);

  // Interns a counter track associated with a cpu into the storage.
  // TODO(mayzner): Remove when all usages migrated to new track design.
  TrackId LegacyInternCpuIdleStateTrack(uint32_t cpu, StringId state);

  // Interns a counter track associated with an irq into the storage.
  // TODO(mayzner): Remove when all usages migrated to new track design.
  TrackId LegacyInternIrqCounterTrack(TrackClassification, int32_t irq);

  // Interns a counter track associated with an softirq into the storage.
  // TODO(mayzner): Remove when all usages migrated to new track design.
  TrackId LegacyInternSoftirqCounterTrack(TrackClassification, int32_t softirq);

  // Interns energy counter track associated with a
  // Energy breakdown into the storage.
  // TODO(mayzner): Remove when all usages migrated to new track design.
  TrackId LegacyInternLegacyEnergyCounterTrack(StringId name,
                                               int32_t consumer_id,
                                               StringId consumer_type,
                                               int32_t ordinal);

  // Interns a per process energy consumer counter track associated with a
  // Energy Uid into the storage.
  // TODO(mayzner): Remove when all usages migrated to new track design.
  TrackId LegacyInternLegacyEnergyPerUidCounterTrack(StringId name,
                                                     int32_t consumer_id,
                                                     int32_t uid);

  // Interns a track associated with a Linux device (where a Linux device
  // implies a kernel-level device managed by a Linux driver).
  // TODO(mayzner): Remove when all usages migrated to new track design.
  TrackId LegacyInternLinuxDeviceTrack(StringId name);

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

  // NOTE:
  // The below method should only be called by AsyncTrackSetTracker

  // Creates and inserts a global async track into the storage.
  // TODO(mayzner): Remove when all usages migrated to new track design.
  TrackId LegacyCreateGlobalAsyncTrack(StringId name, StringId source);

  // Creates and inserts a Android async track into the storage.
  // TODO(mayzner): Remove when all usages migrated to new track design.
  TrackId LegacyCreateProcessAsyncTrack(StringId name,
                                        UniquePid upid,
                                        StringId source);

 private:
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

  Dimensions SingleDimension(StringId key, Variadic val) {
    GlobalArgsTracker::Arg arg;
    arg.flat_key = key;
    arg.key = key;
    arg.value = val;

    // Those will not be used.
    arg.row = std::numeric_limits<uint32_t>::max();
    arg.column = nullptr;

    std::vector<GlobalArgsTracker::Arg> args{arg};

    return Dimensions{context_->global_args_tracker->AddArgSet(args, 0, 1)};
  }

  std::string GetClassification(TrackClassification type) {
    switch (type) {
      case TrackClassification::kTrigger:
        return "triggers";
      case TrackClassification::kInterconnect:
        return "interconnect_events";
      case TrackClassification::kChromeLegacyGlobalInstant:
        return "legacy_chrome_global_instants";
      case TrackClassification::kThread:
        return "thread";
      case TrackClassification::kProcess:
        return "process";
      case TrackClassification::kChromeProcessInstant:
        return "chrome_process_instant";
      case TrackClassification::kLinuxDevice:
        return "linux_device";
      case TrackClassification::kAsync:
        return "async";
      case TrackClassification::kIrqCount:
        return "irq_count_num";
      case TrackClassification::kSoftirqCount:
        return "softirq_count_num";
      case TrackClassification::kGpuFrequency:
        return "gpu_frequency";
      case TrackClassification::kFuncgraphCpu:
        return "cpu_funcgraph";
      case TrackClassification::kIrqCpu:
        return "cpu_irq";
      case TrackClassification::kIrqTime:
        return "cpu_irq_time";
      case TrackClassification::kMaliIrqCpu:
        return "cpu_mali_irq";
      case TrackClassification::kNapiGroCpu:
        return "cpu_napi_gro";
      case TrackClassification::kSoftirqCpu:
        return "cpu_softirq";
      case TrackClassification::kSoftIrqTime:
        return "cpu_softirq_time";
      case TrackClassification::kPkvmHypervisor:
        return "pkvm_hypervisor";
      case TrackClassification::kMaxFreqCpu:
        return "cpu_max_frequency";
      case TrackClassification::kMinFreqCpu:
        return "cpu_min_frequency";
      case TrackClassification::kCpuFrequency:
        return "cpu_frequency";
      case TrackClassification::kCpuFrequencyThrottle:
        return "cpu_frequency_throttle";
      case TrackClassification::kCpuMinFrequencyLimit:
        return "cpu_min_frequency_limit";
      case TrackClassification::kCpuMaxFrequencyLimit:
        return "cpu_max_frequency_limit";
      case TrackClassification::kCpuCapacity:
        return "cpu_capacity";
      case TrackClassification::kCpuIdle:
        return "cpu_idle";
      case TrackClassification::kCpuIdleTime:
        return "cpu_idle_time";
      case TrackClassification::kCpuIdleState:
        return "cpu_idle_state";
      case TrackClassification::kIoWaitTime:
        return "cpu_io_wait_time";
      case TrackClassification::kCpuNumberRunning:
        return "cpu_nr_running";
      case TrackClassification::kCpuUtilization:
        return "cpu_utilization";
      case TrackClassification::kSystemModeTime:
        return "cpu_system_mode_time";
      case TrackClassification::kUserTime:
        return "cpu_user_time";
      case TrackClassification::kNiceUserTime:
        return "cpu_nice_user_time";

      case TrackClassification::kUnknown:
        return "N/A";
    }
    PERFETTO_FATAL("For GCC");
  }

  TrackId InternTrackForGroup(Group);

  TrackId InternProcessTrack(
      TrackClassification,
      UniquePid,
      std::optional<Dimensions> use_other_dimension = std::nullopt,
      StringId name = kNullStringId);

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

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_TRACK_TRACKER_H_
