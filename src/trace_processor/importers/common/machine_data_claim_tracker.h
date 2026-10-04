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

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/hash.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

// Tracks claims on machine-wide data sources (such as scheduling events and
// machine-scoped counters like CPU frequency, CPU idle, battery counters)
// per machine.
//
// The first trace to provide a machine-wide counter on a machine-scoped track
// claims ownership of that track for the entire merge. Similarly,
// context-switch events claim scheduling ownership for the machine.
//
// When conflicting events from subsequent traces are dropped, they are
// recorded in stats::machine_sched_claim_conflict and
// stats::machine_counter_claim_conflict, and an entry is added to import logs
// on the first drop for each stream (and counter type).
//
// What is dropped: sched slices, thread states and duplicate machine counter
// values. What is kept: thread/process lifecycle updates (new thread, end
// thread, renames) and raw ftrace_event rows from every trace, since other kept
// data (e.g. app slices) needs them. Lifecycle updates must be safe to repeat
// when two traces contain the same event (see FtraceParser::ParseTaskNewTask).
//
// Known limitations:
// - Wakeups/new-task events that arrive before any trace's first context
//   switch are kept for each trace, opening runnable states. When the first
//   trace claims scheduling with a context switch, any other trace's open
//   thread states are closed at that switch's timestamp.
//   kOther events don't claim because a trace with only task_newtask (common
//   in process-tracking configs) would otherwise steal scheduling from a full
//   system trace recorded at the same time.
// - Fuchsia scheduler events bypass ThreadStateTracker and are not checked.
class MachineDataClaimTracker {
 public:
  enum class SchedEventKind { kSwitch, kOther };

  explicit MachineDataClaimTracker(TraceProcessorContext* context);
  MachineDataClaimTracker(const MachineDataClaimTracker&) = delete;
  MachineDataClaimTracker& operator=(const MachineDataClaimTracker&) = delete;

  // Registers |track_id| as a machine-scoped counter track with counter |type|.
  // Idempotent; grows internal lookup tables as needed.
  void RegisterMachineTrack(TrackId track_id, std::string_view type);

  // Returns true if counter events on |track_id| should be retained for
  // |caller|. If |track_id| is not a machine track, always returns true.
  // Otherwise, checks or sets ownership of that machine track for
  // caller's trace. If dropped, logs to import_logs_tracker on the first drop
  // for that counter type and increments stats::machine_counter_claim_conflict
  // on |caller|.
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
    if (PERFETTO_LIKELY(claim.owner == trace_id)) {
      return true;
    }
    return self->KeepCounterSlow(caller, track, ts, trace_id);
  }

  // Returns true if scheduling events should be retained for |caller|.
  // kSwitch claims ownership if unclaimed. kOther checks ownership without
  // claiming (returns true if unclaimed or owned by caller; returns false
  // only if owned by another trace). If dropped, logs to import_logs_tracker
  // on the first drop and increments stats::machine_sched_claim_conflict on
  // |caller|.
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
  // been applied by any trace on this machine at the current timestamp,
  // returning the existing |utid|. Does not mutate state.
  std::optional<UniqueTid> FindTaskNewTask(int64_t ts, uint32_t pid) const;

 private:
  struct TrackClaim {
    bool is_machine_track = false;
    StringId type_id;
    std::optional<tables::TraceFileTable::Id> owner;
  };

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

  // Owner of the machine's scheduling stream.
  std::optional<tables::TraceFileTable::Id> sched_owner_;

  // Known contributing trace contexts on this machine.
  base::FlatHashMap<TraceId, TraceProcessorContext*> trace_contexts_;

  // Machine-scoped counter track claims, indexed by TrackId.value.
  std::vector<TrackClaim> track_claims_;

  // Tracks traces that have already logged a drop for sched.
  std::vector<TraceId> sched_dropped_traces_;

  // Tracks (TraceId, counter_type) pairs that have already logged a drop.
  std::vector<std::pair<TraceId, StringId>> counter_dropped_types_;

  // Dedup map for task_newtask events at the current timestamp: pid -> utid.
  int64_t last_newtask_ts_ = std::numeric_limits<int64_t>::min();
  base::FlatHashMap<uint32_t, UniqueTid> applied_newtasks_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_MACHINE_DATA_CLAIM_TRACKER_H_
