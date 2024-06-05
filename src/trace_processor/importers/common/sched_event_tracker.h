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

#include "src/trace_processor/importers/common/event_tracker.h"

#include "src/trace_processor/importers/common/cpu_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/destructible.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

// Tracks per-cpu scheduling events, storing them as slices in the |sched|
// table.
class SchedEventTracker : public Destructible {
 public:
  PERFETTO_ALWAYS_INLINE
  explicit SchedEventTracker(TraceProcessorContext* context)
      : context_(context) {}
  SchedEventTracker(const SchedEventTracker&) = delete;
  ~SchedEventTracker() override;

  PERFETTO_ALWAYS_INLINE
  uint32_t AddStartSlice(uint32_t cpu,
                         int64_t ts,
                         UniqueTid next_utid,
                         int32_t next_prio) {
    // Open a new scheduling slice, corresponding to the task that was
    // just switched to. Set the duration to -1, to indicate that the event is
    // not finished. Duration will be updated later after event finish.
    auto* sched = context_->storage->mutable_sched_slice_table();
    // Get the unique CPU Id over all machines from the CPU table.
    auto ucpu = context_->cpu_tracker->GetOrCreateCpu(cpu);
    auto row_and_id = sched->Insert(
        {ts, /* duration */ -1, next_utid, kNullStringId, next_prio, ucpu});
    SchedId sched_id = row_and_id.id;
    return *sched->id().IndexOf(sched_id);
  }

  PERFETTO_ALWAYS_INLINE
  bool UpdateEventTrackerTimestamp(int64_t ts,
                                   const char* event_name,
                                   size_t stats) {
    // Post sorter stage, all events should be globally timestamp ordered.
    int64_t max_ts = context_->event_tracker->max_timestamp();
    if (ts < max_ts) {
      PERFETTO_ELOG("%s event out of order by %.4f ms, skipping", event_name,
                    static_cast<double>(max_ts - ts) / 1e6);
      context_->storage->IncrementStats(stats);
      return false;
    }
    context_->event_tracker->UpdateMaxTimestamp(ts);
    return true;
  }

  PERFETTO_ALWAYS_INLINE
  void ClosePendingSlice(uint32_t pending_slice_idx,
                         int64_t ts,
                         StringId prev_state) {
    auto* slices = context_->storage->mutable_sched_slice_table();

    int64_t duration = ts - slices->ts()[pending_slice_idx];
    slices->mutable_dur()->Set(pending_slice_idx, duration);

    // We store the state as a uint16 as we only consider values up to 2048
    // when unpacking the information inside; this allows savings of 48 bits
    // per slice.
    slices->mutable_end_state()->Set(pending_slice_idx, prev_state);
  }

 private:
  TraceProcessorContext* const context_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_SCHED_EVENT_TRACKER_H_
