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

#include <optional>
#include <string>

#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/cpu_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

// Tracks and stores tracks based on track types, ids and scopes.
class TrackTracker {
 public:
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

  // Classifications of global tracks (one per trace).
  enum class GlobalTrackType {
    kTrigger,
    kInterconnect,
    kChromeLegacyGlobalInstant
  };

  // Classifications of CPU tracks.
  enum class CpuTrackType {
    kIrqCpu,
    kSortIrqCpu,
    kNapiGroCpu,
    kMaliIrqCpu,
    kMinFreqCpu,
    kMaxFreqCpu,
    kFuncgraphCpu,
    kPkvmHypervisor,
  };

  // Classifications of CPU counter tracks.
  enum class CpuCounterTrackType {
    // Frequency CPU counters
    kFrequency,
    kFreqThrottle,
    kMaxFreqLimit,
    kMinFreqLimit,

    kIdle,
    kIdleState,
    kUtilization,
    kCapacity,
    kNrRunning,

    // Time CPU spent in state.
    kUserTime,
    kNiceUserTime,
    kSystemModeTime,
    kIoWaitTime,
    kIrqTime,
    kSoftIrqTime,
    kIdleTime,
  };

  // Classifications of GPU counter tracks.
  enum class GpuCounterTrackType { kFrequency };

  enum class IrqCounterTrackType { kCount };

  enum class SoftIrqCounterTrackType { kCount };

  using SetArgsCallback = std::function<void(ArgsTracker::BoundInserter&)>;

  explicit TrackTracker(TraceProcessorContext*);

  // Interns a unique track into the storage.
  TrackId InternGlobalTrack(GlobalTrackType);

  // Interns a global track keyed by track type + CPU into the storage.
  TrackId InternCpuTrack(CpuTrackType, uint32_t cpu);

  // Interns a thread track into the storage.
  TrackId InternThreadTrack(UniqueTid utid);

  // Interns a process track into the storage.
  TrackId InternProcessTrack(UniquePid upid);

  // Interns a given GPU track into the storage.
  TrackId InternGpuTrack(const tables::GpuTrackTable::Row& row);

  // Interns a GPU work period track into the storage.
  TrackId InternGpuWorkPeriodTrack(
      const tables::GpuWorkPeriodTrackTable::Row& row);

  // Interns a legacy Chrome async event track into the storage.
  TrackId InternLegacyChromeAsyncTrack(StringId name,
                                       uint32_t upid,
                                       int64_t trace_id,
                                       bool trace_id_is_process_scoped,
                                       StringId source_scope);

  // Interns a track for legacy Chrome process-scoped instant events into the
  // storage.
  TrackId InternLegacyChromeProcessInstantTrack(UniquePid upid);

  // Interns a global counter track into the storage.
  TrackId InternGlobalCounterTrack(Group group,
                                   StringId name,
                                   SetArgsCallback = {},
                                   StringId unit = kNullStringId,
                                   StringId description = kNullStringId);

  // Interns a counter track associated with a cpu into the storage.
  TrackId InternCpuCounterTrack(CpuCounterTrackType, uint32_t cpu);

  // Interns a counter track associated with a GPU into the storage.
  TrackId InternGpuCounterTrack(GpuCounterTrackType, uint32_t gpu_id);

  // Interns a counter track associated with a cpu into the storage.
  TrackId InternCpuIdleStateTrack(uint32_t cpu, StringId state);

  // Interns a counter track associated with a thread into the storage.
  TrackId InternThreadCounterTrack(StringId name, UniqueTid utid);

  // Interns a counter track associated with a process into the storage.
  TrackId InternProcessCounterTrack(StringId name,
                                    UniquePid upid,
                                    StringId unit = kNullStringId,
                                    StringId description = kNullStringId);

  // Interns a counter track associated with an irq into the storage.
  TrackId InternIrqCounterTrack(IrqCounterTrackType, int32_t irq);

  // Interns a counter track associated with an softirq into the storage.
  TrackId InternSoftirqCounterTrack(SoftIrqCounterTrackType, int32_t softirq);

  // Interns energy counter track associated with a
  // Energy breakdown into the storage.
  TrackId InternEnergyCounterTrack(StringId name,
                                   int32_t consumer_id,
                                   StringId consumer_type,
                                   int32_t ordinal);

  // Interns a per process energy consumer counter track associated with a
  // Energy Uid into the storage.
  TrackId InternEnergyPerUidCounterTrack(StringId name,
                                         int32_t consumer_id,
                                         int32_t uid);

  // Interns a track associated with a Linux device (where a Linux device
  // implies a kernel-level device managed by a Linux driver).
  TrackId InternLinuxDeviceTrack(StringId name);

  // Creates a counter track associated with a GPU into the storage.
  TrackId CreateGpuCounterTrack(StringId name,
                                uint32_t gpu_id,
                                StringId description = StringId::Null(),
                                StringId unit = StringId::Null());

  // Creates a counter track for values within perf samples.
  // The tracks themselves are managed by PerfSampleTracker.
  TrackId CreatePerfCounterTrack(StringId name,
                                 tables::PerfSessionTable::Id perf_session_id,
                                 uint32_t cpu,
                                 bool is_timebase);

  // Interns a Fuchsia async track into the storage.
  TrackId InternFuchsiaAsyncTrack(StringId name,
                                  uint32_t upid,
                                  int64_t correlation_id);

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
  struct GpuWorkPeriodTrackTuple {
    StringId track_name;
    uint32_t gpu_id;
    int32_t uid;

    friend bool operator<(const GpuWorkPeriodTrackTuple& l,
                          const GpuWorkPeriodTrackTuple& r) {
      return std::tie(l.track_name, l.gpu_id, l.uid) <
             std::tie(r.track_name, r.gpu_id, r.uid);
    }
  };
  struct ChromeTrackTuple {
    std::optional<int64_t> upid;
    int64_t trace_id = 0;
    StringId source_scope = StringId::Null();

    friend bool operator<(const ChromeTrackTuple& l,
                          const ChromeTrackTuple& r) {
      return std::tie(l.trace_id, l.upid, l.source_scope) <
             std::tie(r.trace_id, r.upid, r.source_scope);
    }
  };
  struct CpuCounterTrackTuple {
    // Required fields.
    CpuCounterTrackType type;
    uint32_t cpu;
    StringId name = StringPool::Id::Null();

    // Optional properties of the track.
    uint64_t optional_hash = 0;

    friend bool operator<(const CpuCounterTrackTuple& l,
                          const CpuCounterTrackTuple& r) {
      return std::tie(l.type, l.cpu, l.optional_hash) <
             std::tie(r.type, r.cpu, r.optional_hash);
    }
  };
  static constexpr size_t kGroupCount =
      static_cast<uint32_t>(Group::kSizeSentinel);

  std::string GetClassification(TrackTracker::GlobalTrackType type) {
    switch (type) {
      case TrackTracker::GlobalTrackType::kTrigger:
        return "triggers";
      case TrackTracker::GlobalTrackType::kInterconnect:
        return "interconnect_events";
      case TrackTracker::GlobalTrackType::kChromeLegacyGlobalInstant:
        return "legacy_chrome_global_instants";
    }
    PERFETTO_FATAL("For GCC");
  }

  std::string GetClassification(TrackTracker::CpuTrackType type) {
    switch (type) {
      case CpuTrackType::kIrqCpu:
        return "irq";
      case CpuTrackType::kSortIrqCpu:
        return "soft_irq";
      case CpuTrackType::kNapiGroCpu:
        return "napi_gro";
      case CpuTrackType::kMaliIrqCpu:
        return "mali_irq";
      case CpuTrackType::kMinFreqCpu:
        return "min_freq";
      case CpuTrackType::kMaxFreqCpu:
        return "max_freq";
      case CpuTrackType::kFuncgraphCpu:
        return "funcgraph";
      case CpuTrackType::kPkvmHypervisor:
        return "pkvm_hypervisor";
    }
    PERFETTO_FATAL("For GCC");
  }

  std::string GetClassification(TrackTracker::CpuCounterTrackType type) {
    switch (type) {
      case CpuCounterTrackType::kFrequency:
        return "frequency";
      case CpuCounterTrackType::kFreqThrottle:
        return "frequency_throttle";
      case CpuCounterTrackType::kMaxFreqLimit:
        return "max_frequency_limit";
      case CpuCounterTrackType::kMinFreqLimit:
        return "min_frequency_limit";
      case CpuCounterTrackType::kIdle:
        return "idle";
      case CpuCounterTrackType::kIdleState:
        return "idle_state";
      case CpuCounterTrackType::kIdleTime:
        return "idle_time";
      case CpuCounterTrackType::kUtilization:
        return "utilization";
      case CpuCounterTrackType::kCapacity:
        return "capacity";
      case CpuCounterTrackType::kNrRunning:
        return "nr_running";
      case CpuCounterTrackType::kUserTime:
        return "user_time";
      case CpuCounterTrackType::kNiceUserTime:
        return "nice_user_time";
      case CpuCounterTrackType::kSystemModeTime:
        return "system_mode_time";
      case CpuCounterTrackType::kIoWaitTime:
        return "io_wait_time";
      case CpuCounterTrackType::kIrqTime:
        return "irq_time";
      case CpuCounterTrackType::kSoftIrqTime:
        return "soft_irq_time";
    }
    PERFETTO_FATAL("For GCC");
  }

  std::string GetClassification(TrackTracker::GpuCounterTrackType type) {
    switch (type) {
      case GpuCounterTrackType::kFrequency:
        return "frequency";
    }
    PERFETTO_FATAL("For GCC");
  }

  std::string GetClassification(TrackTracker::IrqCounterTrackType type) {
    switch (type) {
      case IrqCounterTrackType::kCount:
        return "count";
    }
    PERFETTO_FATAL("For GCC");
  }

  std::string GetClassification(TrackTracker::SoftIrqCounterTrackType type) {
    switch (type) {
      case SoftIrqCounterTrackType::kCount:
        return "count";
    }
    PERFETTO_FATAL("For GCC");
  }

  TrackId InternTrackForGroup(Group);
  TrackId InternCpuCounterTrack(CpuCounterTrackTuple);

  std::array<std::optional<TrackId>, kGroupCount> group_track_ids_;

  std::map<UniqueTid, TrackId> thread_tracks_;
  std::map<UniquePid, TrackId> process_tracks_;
  std::map<int64_t /* correlation_id */, TrackId> fuchsia_async_tracks_;

  std::map<std::pair<StringId, uint32_t /* cpu */>, TrackId> cpu_tracks_;

  std::map<GpuTrackTuple, TrackId> gpu_tracks_;
  std::map<ChromeTrackTuple, TrackId> chrome_tracks_;
  std::map<UniquePid, TrackId> chrome_process_instant_tracks_;
  std::map<std::pair<StringId, int32_t /*uid*/>, TrackId> uid_tracks_;
  std::map<GpuWorkPeriodTrackTuple, TrackId> gpu_work_period_tracks_;

  std::map<StringId, TrackId> global_counter_tracks_by_name_;
  std::map<CpuCounterTrackTuple, TrackId> cpu_counter_tracks_;
  std::map<std::pair<GpuCounterTrackType, uint32_t>, TrackId>
      gpu_counter_tracks_;
  std::map<std::pair<StringId, UniqueTid>, TrackId> utid_counter_tracks_;
  std::map<std::pair<StringId, UniquePid>, TrackId> upid_counter_tracks_;
  std::map<std::pair<IrqCounterTrackType, int32_t>, TrackId>
      irq_counter_tracks_;
  std::map<std::pair<SoftIrqCounterTrackType, int32_t>, TrackId>
      softirq_counter_tracks_;
  std::map<std::pair<StringId, int32_t>, TrackId> energy_counter_tracks_;
  std::map<std::pair<StringId, int32_t>, TrackId> uid_counter_tracks_;
  std::map<std::pair<StringId, int32_t>, TrackId>
      energy_per_uid_counter_tracks_;
  std::map<StringId, TrackId> linux_device_tracks_;

  std::map<GlobalTrackType, TrackId> unique_tracks_;

  const StringId source_key_ = kNullStringId;
  const StringId trace_id_key_ = kNullStringId;
  const StringId trace_id_is_process_scoped_key_ = kNullStringId;
  const StringId source_scope_key_ = kNullStringId;
  const StringId category_key_ = kNullStringId;

  const StringId fuchsia_source_ = kNullStringId;
  const StringId chrome_source_ = kNullStringId;

  TraceProcessorContext* const context_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_TRACK_TRACKER_H_
