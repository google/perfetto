/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_MACHINE_DATA_CLAIM_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_MACHINE_DATA_CLAIM_TRACKER_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

// Decides which trace writes machine-wide data when several traces of one
// machine are merged (zip/tar). One instance per machine.
//
// Destinations: scheduling (sched_slice and thread_state), and each
// machine-scoped counter track (e.g. CPU 2 frequency, battery charge).
//
// Each destination has at most one owner at a time:
//  - The first trace whose write is kept (at parse time) becomes the owner.
//    Writes the parser skips (ftrace soft/hard drops etc.) never claim.
//  - The owner holds the destination until its lease ends: the last
//    timestamp, seen while tokenizing, of the trace's streams that can write
//    the destination (sched events for scheduling; ftrace, sys_stats,
//    battery, ... per counter type, see RegisterMachineTrack()). Leases are
//    only final once every file is tokenized (archives are fully sorted).
//    Before that, or for a trace with no recorded bounds (e.g. JSON), the
//    lease never ends, as in a whole-trace claim.
//  - Another trace's write inside the owner's lease is dropped and counted
//    in machine_sched_claim_conflict / machine_counter_claim_conflict. The
//    first drop per trace (and counter type) is also in the import logs.
//  - Another trace's write after the owner's lease makes it the new owner.
//    For scheduling, the old owner's open slices and thread states are closed
//    at its lease end through its own trackers. For a counter track, the next
//    trace to write it takes over.
//  - Wakeups, new-task runnable states and blocked reasons never claim. While
//    another trace owns scheduling they are dropped. Before anyone owns it,
//    they are dropped only if an earlier-starting trace's sched events cover
//    their timestamp, and the first claim closes the other traces' open
//    thread states.
//
// Limitations:
//  - Equal first timestamps: ties follow the sorter's order.
//  - A destination that stops while its trace's other streams go on (e.g.
//    one counter stops, ftrace continues) blocks other traces until the
//    lease ends. Gaps inside a lease (ftrace data loss) are not filled.
//  - After a handover, each CPU or thread is empty until the new owner's next
//    event for it.
//  - The sched lease of a trace with no sched closer (generic-kernel-only,
//    ETW) never ends, as its open slices can't be closed. JSON traces record
//    no bounds, so they never hand over either. Fuchsia sched writes are not
//    checked.
//  - Different destinations can have different owners at the same time.
//  - Dropped switches still create threads, update names and add raw
//    ftrace_event rows.
//  - Traces from different boots must be given different machines in the
//    manifest.
class MachineDataClaimTracker {
 public:
  enum class SchedEventKind { kSwitch, kOther };

  enum class SourceKind : uint8_t {
    kFtrace = 0,
    kEtw,
    kSystrace,
    kSysStats,
    kBattery,
    kEntityState,
    kGenericKernel,
    kDumpstateBattery,
    kCount
  };
  static constexpr size_t kSourceKindCount =
      static_cast<size_t>(SourceKind::kCount);

  // Source masks for counter blueprints
  static constexpr uint32_t kSourceFtrace =
      1u << static_cast<uint8_t>(SourceKind::kFtrace);
  static constexpr uint32_t kSourceEtw =
      1u << static_cast<uint8_t>(SourceKind::kEtw);
  static constexpr uint32_t kSourceSystrace =
      1u << static_cast<uint8_t>(SourceKind::kSystrace);
  static constexpr uint32_t kSourceSysStats =
      1u << static_cast<uint8_t>(SourceKind::kSysStats);
  static constexpr uint32_t kSourceBattery =
      1u << static_cast<uint8_t>(SourceKind::kBattery);
  static constexpr uint32_t kSourceEntityState =
      1u << static_cast<uint8_t>(SourceKind::kEntityState);
  static constexpr uint32_t kSourceGenericKernel =
      1u << static_cast<uint8_t>(SourceKind::kGenericKernel);
  static constexpr uint32_t kSourceDumpstateBattery =
      1u << static_cast<uint8_t>(SourceKind::kDumpstateBattery);

  explicit MachineDataClaimTracker(TraceProcessorContext* context);
  ~MachineDataClaimTracker();
  MachineDataClaimTracker(const MachineDataClaimTracker&) = delete;
  MachineDataClaimTracker& operator=(const MachineDataClaimTracker&) = delete;

  // Called during tokenization to note a timestamp for a given source kind.
  static PERFETTO_ALWAYS_INLINE void NoteData(TraceProcessorContext* context,
                                              SourceKind kind,
                                              int64_t ts) {
    if (PERFETTO_LIKELY(context && context->machine_data_claim_tracker)) {
      context->machine_data_claim_tracker->NoteDataImpl(context, kind, ts);
    }
  }

  // Called during tokenization to note a timestamp for a sched event.
  static PERFETTO_ALWAYS_INLINE void NoteSchedData(
      TraceProcessorContext* context,
      int64_t ts) {
    if (PERFETTO_LIKELY(context && context->machine_data_claim_tracker)) {
      context->machine_data_claim_tracker->NoteSchedDataImpl(context, ts);
    }
  }

  static std::optional<SourceKind> SourceKindForTracePacketField(
      uint32_t field_id);

  // Called when all files have been tokenized before parse begins.
  void OnTokenizationDone();
  static void NotifyTokenizationDone(TraceProcessorContext* context);

  using SchedCloser = std::function<void(int64_t)>;
  void RegisterSchedCloser(TraceId trace_id, SchedCloser closer) {
    sched_closers_[trace_id] = std::move(closer);
  }

  // Registers |track_id| as a machine-scoped counter track with counter |type|.
  // Idempotent; grows internal lookup tables as needed.
  void RegisterMachineTrack(TrackId track_id, std::string_view type);

  // Checks whether counter sample on |track| should be kept.
  PERFETTO_ALWAYS_INLINE static bool KeepCounter(TraceProcessorContext* caller,
                                                 TrackId track,
                                                 int64_t ts) {
    auto* self = caller->machine_data_claim_tracker.get();
    if (PERFETTO_UNLIKELY(!self)) {
      return true;
    }
    if (PERFETTO_LIKELY(track.value >= self->track_claims_.size())) {
      return true;
    }
    const auto& claim = self->track_claims_[track.value];
    if (PERFETTO_LIKELY(!claim.is_machine_track)) {
      return true;
    }
    TraceId trace_id = caller->trace_state ? caller->trace_state->trace_id
                                           : caller->trace_id();
    if (PERFETTO_LIKELY(claim.owner == trace_id &&
                        ts <= claim.owner_lease_end)) {
      return true;
    }
    return self->KeepCounterSlow(caller, track, ts, trace_id);
  }

  // Checks whether scheduling event should be kept.
  PERFETTO_ALWAYS_INLINE static bool KeepSched(TraceProcessorContext* caller,
                                               SchedEventKind kind,
                                               int64_t ts) {
    auto* self = caller->machine_data_claim_tracker.get();
    if (PERFETTO_UNLIKELY(!self)) {
      return true;
    }
    TraceId trace_id = caller->trace_state ? caller->trace_state->trace_id
                                           : caller->trace_id();
    if (PERFETTO_LIKELY(self->sched_owner_ == trace_id)) {
      return true;
    }
    return self->KeepSchedSlow(caller, kind, ts, trace_id);
  }

  // Records that a task_newtask event at |ts| for child |pid| was applied to
  // create |utid|. Clears older entries when |ts| advances.
  void RecordTaskNewTask(int64_t ts, uint32_t pid, UniqueTid utid);

  // Looks up whether a task_newtask event at |ts| for child |pid| has already
  // been applied by any trace on this machine at the current timestamp.
  std::optional<UniqueTid> FindTaskNewTask(int64_t ts, uint32_t pid) const;

  // Testing helpers
  std::optional<TraceId> sched_owner_for_testing() const {
    return sched_owner_;
  }
  int64_t sched_owner_lease_end_for_testing() const {
    return sched_owner_ ? SchedLeaseEnd(*sched_owner_)
                        : std::numeric_limits<int64_t>::min();
  }
  std::optional<TraceId> counter_owner_for_testing(TrackId track) const {
    if (track.value < track_claims_.size()) {
      return track_claims_[track.value].owner;
    }
    return std::nullopt;
  }
  int64_t counter_owner_lease_end_for_testing(TrackId track) const {
    if (track.value < track_claims_.size()) {
      return track_claims_[track.value].owner_lease_end;
    }
    return std::numeric_limits<int64_t>::min();
  }

 private:
  struct KindBounds {
    int64_t min = std::numeric_limits<int64_t>::max();
    int64_t max = std::numeric_limits<int64_t>::min();
    bool valid = false;
  };

  struct TraceBounds {
    TraceId trace_id{0};
    int64_t overall_min = std::numeric_limits<int64_t>::max();
    int64_t overall_max = std::numeric_limits<int64_t>::min();
    bool overall_valid = false;

    int64_t sched_min = std::numeric_limits<int64_t>::max();
    int64_t sched_max = std::numeric_limits<int64_t>::min();
    bool sched_valid = false;

    std::array<KindBounds, kSourceKindCount> kinds;
  };

  struct TrackClaim {
    bool is_machine_track = false;
    StringId type_id;
    uint32_t source_mask = 0;
    std::optional<TraceId> owner;
    int64_t owner_lease_end = std::numeric_limits<int64_t>::min();
  };

  PERFETTO_ALWAYS_INLINE void NoteDataImpl(TraceProcessorContext* context,
                                           SourceKind kind,
                                           int64_t ts) {
    TraceBounds* tb = (PERFETTO_LIKELY(last_noted_context_ == context))
                          ? last_trace_bounds_
                          : GetOrCreateTraceBounds(context);
    size_t k = static_cast<size_t>(kind);
    tb->kinds[k].min = std::min(tb->kinds[k].min, ts);
    tb->kinds[k].max = std::max(tb->kinds[k].max, ts);
    tb->kinds[k].valid = true;
    tb->overall_min = std::min(tb->overall_min, ts);
    tb->overall_max = std::max(tb->overall_max, ts);
    tb->overall_valid = true;
  }

  PERFETTO_ALWAYS_INLINE void NoteSchedDataImpl(TraceProcessorContext* context,
                                                int64_t ts) {
    TraceBounds* tb = (PERFETTO_LIKELY(last_noted_context_ == context))
                          ? last_trace_bounds_
                          : GetOrCreateTraceBounds(context);
    tb->sched_min = std::min(tb->sched_min, ts);
    tb->sched_max = std::max(tb->sched_max, ts);
    tb->sched_valid = true;
    tb->overall_min = std::min(tb->overall_min, ts);
    tb->overall_max = std::max(tb->overall_max, ts);
    tb->overall_valid = true;
  }

  TraceBounds* GetOrCreateTraceBounds(TraceProcessorContext* context);
  const TraceBounds* FindTraceBounds(TraceId trace_id) const;

  int64_t SchedLeaseEnd(TraceId trace_id) const;
  int64_t CounterLeaseEnd(TraceId trace_id, uint32_t source_mask) const;

  void CloseOpenRowsForSched(TraceId old_owner, int64_t close_ts);
  std::optional<TraceId> EarlierSchedTraceCovering(TraceId trace_id,
                                                   int64_t ts) const;
  void RecordSchedDrop(TraceProcessorContext* caller,
                       TraceId trace_id,
                       int64_t ts,
                       TraceId owner_id);

  bool KeepCounterSlow(TraceProcessorContext* caller,
                       TrackId track,
                       int64_t ts,
                       TraceId trace_id);
  bool KeepSchedSlow(TraceProcessorContext* caller,
                     SchedEventKind kind,
                     int64_t ts,
                     TraceId trace_id);

  TraceProcessorContext* const context_;
  const StringId kind_key_id_;
  const StringId counter_type_key_id_;
  const StringId sched_kind_id_;
  const StringId counter_kind_id_;
  const StringId owner_trace_id_key_id_;

  bool tokenization_done_ = false;
  const TraceProcessorContext* last_noted_context_ = nullptr;
  TraceBounds* last_trace_bounds_ = nullptr;
  std::vector<TraceBounds> trace_bounds_;
  base::FlatHashMap<TraceId, TraceProcessorContext*> trace_contexts_;
  base::FlatHashMap<TraceId, SchedCloser> sched_closers_;

  std::optional<TraceId> sched_owner_;

  std::vector<TrackClaim> track_claims_;

  std::vector<TraceId> sched_dropped_traces_;
  std::vector<std::pair<TraceId, StringId>> counter_dropped_types_;

  int64_t last_newtask_ts_ = std::numeric_limits<int64_t>::min();
  base::FlatHashMap<uint32_t, UniqueTid> applied_newtasks_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_MACHINE_DATA_CLAIM_TRACKER_H_
