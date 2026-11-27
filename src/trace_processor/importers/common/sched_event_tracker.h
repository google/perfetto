/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_SCHED_EVENT_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_SCHED_EVENT_TRACKER_H_

#include <cstdint>

#include "perfetto/public/compiler.h"
#include "src/trace_processor/importers/common/cpu_tracker.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/sched_event_state.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/sched_tables_py.h"
#include "src/trace_processor/types/destructible.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

// Tracks per-cpu scheduling events, storing them as slices in the |sched|
// table.
class SchedEventTracker : public Destructible {
 public:
  PERFETTO_ALWAYS_INLINE
  explicit SchedEventTracker(TraceProcessorContext* context)
      : context_(context) {}
  SchedEventTracker(const SchedEventTracker&) = delete;
  ~SchedEventTracker() override;

  // Opens a scheduling slice for the task just switched to on `cpu`, and makes
  // it `pending`'s pending slice. Its duration is -1 until it is closed.
  PERFETTO_ALWAYS_INLINE
  void AddStartSlice(SchedEventState::PendingSchedInfo* pending,
                     uint32_t cpu,
                     int64_t ts,
                     UniqueTid next_utid,
                     int32_t next_prio) {
    auto* sched = context_->storage->mutable_sched_slice_table();
    // Get the unique CPU Id over all machines from the CPU table.
    auto ucpu = context_->cpu_tracker->GetOrCreateCpu(cpu);
    auto row_and_id = sched->Insert(
        {ts, /* duration */ -1, next_utid, kNullStringId, next_prio, ucpu});
    pending->pending_slice_storage_idx = row_and_id.row;
    pending->pending_slice_ts = ts;
    pending->pending_slice_end_ts = -1;
  }

  PERFETTO_ALWAYS_INLINE
  void ClosePendingSlice(SchedEventState::PendingSchedInfo* pending,
                         int64_t ts,
                         StringId prev_state) {
    auto* slices = context_->storage->mutable_sched_slice_table();
    auto r = (*slices)[pending->pending_slice_storage_idx];
    r.set_dur(ts - pending->pending_slice_ts);
    r.set_end_state(prev_state);
    pending->pending_slice_end_ts = ts;
  }

  PERFETTO_ALWAYS_INLINE
  void SetEndStateForPendingSlice(
      const SchedEventState::PendingSchedInfo& pending,
      StringId prev_state) {
    auto* slices = context_->storage->mutable_sched_slice_table();
    (*slices)[pending.pending_slice_storage_idx].set_end_state(prev_state);
  }

 private:
  TraceProcessorContext* const context_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_SCHED_EVENT_TRACKER_H_
