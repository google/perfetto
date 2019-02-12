/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/trace_processor/event_tracker.h"
#include "perfetto/base/utils.h"
#include "src/trace_processor/ftrace_utils.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/stats.h"
#include "src/trace_processor/trace_processor_context.h"

#include <math.h>

namespace perfetto {
namespace trace_processor {

EventTracker::EventTracker(TraceProcessorContext* context)
    : idle_string_id_(context->storage->InternString("idle")),
      context_(context) {}

EventTracker::~EventTracker() = default;

StringId EventTracker::GetThreadNameId(uint32_t tid, base::StringView comm) {
  return tid == 0 ? idle_string_id_ : context_->storage->InternString(comm);
}

void EventTracker::PushSchedSwitch(uint32_t cpu,
                                   int64_t timestamp,
                                   uint32_t prev_pid,
                                   int64_t prev_state,
                                   uint32_t next_pid,
                                   base::StringView next_comm,
                                   int32_t next_priority) {
  // At this stage all events should be globally timestamp ordered.
  if (timestamp < prev_timestamp_) {
    PERFETTO_ELOG("sched_switch event out of order by %.4f ms, skipping",
                  (prev_timestamp_ - timestamp) / 1e6);
    context_->storage->IncrementStats(stats::sched_switch_out_of_order);
    return;
  }
  prev_timestamp_ = timestamp;
  PERFETTO_DCHECK(cpu < base::kMaxCpus);

  auto* slices = context_->storage->mutable_slices();
  auto* pending_slice = &pending_sched_per_cpu_[cpu];
  if (pending_slice->storage_index < std::numeric_limits<size_t>::max()) {
    size_t idx = pending_slice->storage_index;
    int64_t duration = timestamp - slices->start_ns()[idx];
    slices->set_duration(idx, duration);

    if (prev_pid == pending_slice->pid) {
      // We store the state as a uint16 as we only consider values up to 2048
      // when unpacking the information inside; this allows savings of 48 bits
      // per slice.
      slices->set_end_state(
          idx, ftrace_utils::TaskState(static_cast<uint16_t>(prev_state)));
    } else {
      // If the this events previous pid does not match the previous event's
      // next pid, make a note of this.
      context_->storage->IncrementStats(stats::mismatched_sched_switch_tids);
    }
  }

  StringId name_id = GetThreadNameId(next_pid, next_comm);
  auto utid =
      context_->process_tracker->UpdateThread(timestamp, next_pid, name_id);

  pending_slice->storage_index =
      slices->AddSlice(cpu, timestamp, 0 /* duration */, utid,
                       ftrace_utils::TaskState(), next_priority);
  pending_slice->pid = next_pid;
}

RowId EventTracker::PushCounter(int64_t timestamp,
                                double value,
                                StringId name_id,
                                int64_t ref,
                                RefType ref_type) {
  if (timestamp < prev_timestamp_) {
    PERFETTO_ELOG("counter event out of order by %.4f ms, skipping",
                  (prev_timestamp_ - timestamp) / 1e6);
    context_->storage->IncrementStats(stats::counter_events_out_of_order);
    return kInvalidRowId;
  }
  prev_timestamp_ = timestamp;

  auto* counters = context_->storage->mutable_counters();
  size_t idx = counters->AddCounter(timestamp, name_id, value, ref, ref_type);
  return TraceStorage::CreateRowId(TableId::kCounters,
                                   static_cast<uint32_t>(idx));
}

}  // namespace trace_processor
}  // namespace perfetto
