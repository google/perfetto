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
#include <optional>
#include <string_view>
#include <vector>

#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/metadata_tables_py.h"

namespace perfetto::trace_processor {

class TraceProcessorContext;

// Tracks claims on machine-wide data sources (such as scheduling events and
// machine-scoped counters like CPU frequency and CPU idle) per machine.
//
// The first trace to provide a kind of machine-wide data on a machine claims
// ownership of that stream. Only context-switch events (sched_switch, compact
// sched_switch, generic task state) claim scheduling ownership; secondary
// events (sched_waking, new task, blocked reason) never claim ownership on
// their own, and are only dropped once another trace has claimed scheduling.
//
// When conflicting events from subsequent traces are dropped, they are
// recorded in stats::machine_sched_claim_conflict and
// stats::machine_counter_claim_conflict.
//
// What is dropped: sched slices, thread states and machine counter values.
// What is kept: thread/process lifecycle updates (new thread, end thread,
// renames) and raw ftrace_event rows from every trace, since other kept data
// (e.g. app slices) needs them. Lifecycle updates must be safe to repeat when
// two traces contain the same event (see FtraceParser::ParseTaskNewTask).
//
// Known limitations:
// - Wakeups/new-task events that arrive before any trace's first context
//   switch are kept for every trace. If that trace later loses, its Runnable
//   state is never closed (and if it has no later events, no stat is set).
//   kOther events don't claim because a trace with only task_newtask (common
//   in process-tracking configs) would otherwise steal scheduling from a full
//   system trace recorded at the same time.
// - ETW context switches are not gated, but ETW wakeups go through
//   ThreadStateTracker and are.
class MachineDataClaimTracker {
 public:
  enum class SchedEventKind { kSwitch, kOther };

  explicit MachineDataClaimTracker(TraceProcessorContext*);

  // Associates |track_id| with a machine counter category corresponding to
  // |type|. Idempotent; grows internal lookup tables as needed.
  void RegisterMachineTrack(TrackId track_id, std::string_view type);

  // Returns true if counter events on |track_id| should be retained for
  // |caller|. If |track_id| is not a machine track, always returns true.
  // Otherwise, checks or sets ownership of the track's category for
  // caller->trace_id(). If dropped, increments
  // stats::machine_counter_claim_conflict on |caller|.
  static bool KeepCounter(TraceProcessorContext* caller, TrackId track);

  // Returns true if scheduling events should be retained for |caller|.
  // kSwitch claims ownership if unclaimed. kOther checks ownership without
  // claiming (returns true if unclaimed or owned by caller; returns false
  // only if owned by another trace). If dropped, increments
  // stats::machine_sched_claim_conflict on |caller|.
  static bool KeepSched(TraceProcessorContext* caller, SchedEventKind kind);

 private:
  // owner_[0] = sched category owner.
  // owner_[i + 1] = owner of counter_types_[i].
  std::vector<std::optional<tables::TraceFileTable::Id>> owner_;

  // Counter types registered for this machine (category i + 1). These point
  // at blueprint type names, which are string literals with static lifetime.
  std::vector<std::string_view> counter_types_;

  // Indexed by TrackId.value. 0 means "not a machine track".
  // Values 1..N correspond to counter categories (index in counter_types_ + 1).
  std::vector<uint8_t> track_category_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_MACHINE_DATA_CLAIM_TRACKER_H_
