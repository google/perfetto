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

StringId SchedTracker::GetThreadNameId(uint32_t tid, base::StringView comm) {
  return tid == 0 ? idle_string_id_ : context_->storage->InternString(comm);
}

void SchedTracker::PushSchedSwitch(uint32_t cpu,
                                   uint64_t timestamp,
                                   uint32_t prev_pid,
                                   uint32_t,
                                   uint32_t next_pid,
                                   base::StringView next_comm) {
  // At this stage all events should be globally timestamp ordered.
  if (timestamp < prev_timestamp_) {
    PERFETTO_ELOG("sched_switch event out of order by %.4f ms, skipping",
                  (prev_timestamp_ - timestamp) / 1e6);
    return;
  }
  prev_timestamp_ = timestamp;
  PERFETTO_DCHECK(cpu < base::kMaxCpus);

  auto* slices = context_->storage->mutable_slices();
  auto* pending_slice = &pending_sched_per_cpu_[cpu];
  if (pending_slice->storage_index < std::numeric_limits<size_t>::max()) {
    // If the this events previous pid does not match the previous event's next
    // pid, make a note of this.
    if (prev_pid != pending_slice->pid) {
      context_->storage->AddMismatchedSchedSwitch();
    }

    size_t idx = pending_slice->storage_index;
    uint64_t duration = timestamp - slices->start_ns()[idx];
    slices->set_duration(idx, duration);
  }

  StringId name_id = GetThreadNameId(next_pid, next_comm);
  auto utid =
      context_->process_tracker->UpdateThread(timestamp, next_pid, name_id);

  pending_slice->storage_index =
      slices->AddSlice(cpu, timestamp, 0 /* duration */, utid);
  pending_slice->pid = next_pid;
}

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

  auto* counters = context_->storage->mutable_counters();
  const auto& key = CounterKey{ref, name_id};
  auto counter_it = pending_counters_per_key_.find(key);
  if (counter_it != pending_counters_per_key_.end()) {
    size_t idx = counter_it->second;

    uint64_t duration = timestamp - counters->timestamps()[idx];
    double value_delta = value - counters->values()[idx];
    counters->set_duration(idx, duration);
    counters->set_value_delta(idx, value_delta);
  }

  pending_counters_per_key_[key] = counters->AddCounter(
      timestamp, 0 /* duration */, name_id, value, 0 /* value_delta */,
      static_cast<int64_t>(ref), ref_type);
}

}  // namespace trace_processor
}  // namespace perfetto
