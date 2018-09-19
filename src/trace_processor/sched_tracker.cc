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

#include "src/trace_processor/sched_tracker.h"
#include "perfetto/base/utils.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/trace_processor_context.h"

#include <math.h>

namespace perfetto {
namespace trace_processor {

SchedTracker::SchedTracker(TraceProcessorContext* context)
    : idle_string_id_(context->storage->InternString("idle")),
      context_(context) {}

SchedTracker::~SchedTracker() = default;

void SchedTracker::PushSchedSwitch(uint32_t cpu,
                                   uint64_t timestamp,
                                   uint32_t prev_pid,
                                   uint32_t prev_state,
                                   base::StringView prev_comm,
                                   uint32_t next_pid) {
  // At this stage all events should be globally timestamp ordered.
  if (timestamp < prev_timestamp_) {
    PERFETTO_ELOG("sched_switch event out of order by %.4f ms, skipping",
                  (prev_timestamp_ - timestamp) / 1e6);
    return;
  }
  prev_timestamp_ = timestamp;
  PERFETTO_DCHECK(cpu < base::kMaxCpus);
  SchedSwitchEvent* prev = &last_sched_per_cpu_[cpu];
  // If we had a valid previous event, then inform the storage about the
  // slice.
  if (prev->valid()) {
    uint64_t duration = timestamp - prev->timestamp;
    StringId prev_thread_name_id =
        prev->next_pid == 0 ? idle_string_id_
                            : context_->storage->InternString(prev_comm);
    UniqueTid utid = context_->process_tracker->UpdateThread(
        prev->timestamp, prev->next_pid /* == prev_pid */, prev_thread_name_id);
    context_->storage->AddSliceToCpu(cpu, prev->timestamp, duration, utid);
  }

  // If the this events previous pid does not match the previous event's next
  // pid, make a note of this.
  if (prev_pid != prev->next_pid) {
    context_->storage->AddMismatchedSchedSwitch();
  }

  // Update the map with the current event.
  prev->timestamp = timestamp;
  prev->prev_pid = prev_pid;
  prev->prev_state = prev_state;
  prev->next_pid = next_pid;
};

void SchedTracker::PushCounter(uint64_t timestamp,
                               double value,
                               StringId name_id,
                               uint64_t ref,
                               RefType ref_type) {
  if (timestamp < prev_timestamp_) {
    PERFETTO_ELOG("counter event out of order by %.4f ms, skipping",
                  (prev_timestamp_ - timestamp) / 1e6);
    return;
  }
  prev_timestamp_ = timestamp;

  // The previous counter with the same ref and name_id.
  Counter& prev = prev_counters_[CounterKey{ref, name_id}];

  uint64_t duration = 0;
  double value_delta = 0;

  if (prev.timestamp != 0) {
    duration = timestamp - prev.timestamp;
    value_delta = value - prev.value;

    context_->storage->mutable_counters()->AddCounter(
        prev.timestamp, duration, name_id, prev.value, value_delta,
        static_cast<int64_t>(ref), ref_type);
  }

  prev.timestamp = timestamp;
  prev.value = value;
};

}  // namespace trace_processor
}  // namespace perfetto
