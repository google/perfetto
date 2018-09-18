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
    uint64_t cycles = CalculateCycles(cpu, prev->timestamp, timestamp);
    context_->storage->AddSliceToCpu(cpu, prev->timestamp, duration, utid,
                                     cycles);
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

uint64_t SchedTracker::CalculateCycles(uint32_t cpu,
                                       uint64_t start_ns,
                                       uint64_t end_ns) {
  const auto& frequencies = context_->storage->GetFreqForCpu(cpu);
  auto lower_index = lower_index_per_cpu_[cpu];
  if (frequencies.empty())
    return 0;

  long double cycles = 0;

  // Move the lower index up to the first cpu_freq event before start_ns.
  while (lower_index + 1 < frequencies.size()) {
    if (frequencies[lower_index + 1].first >= start_ns)
      break;
    ++lower_index;
  };

  // Since events are processed in timestamp order, we don't have any cpu_freq
  // events with a timestamp larger than end_ns. Therefore we care about all
  // freq events from lower_index (first event before start_ns) to the last
  // cpu_freq event.
  for (size_t i = lower_index; i < frequencies.size(); ++i) {
    // Using max handles the special case for the first cpu_freq event.
    uint64_t cycle_start = std::max(frequencies[i].first, start_ns);
    // If there are no more freq_events we compute cycles until |end_ns|.
    uint64_t cycle_end = end_ns;
    if (i + 1 < frequencies.size())
      cycle_end = frequencies[i + 1].first;

    uint32_t freq_khz = frequencies[i].second;
    cycles += ((cycle_end - cycle_start) / 1E6L) * freq_khz;
  }

  lower_index_per_cpu_[cpu] = frequencies.size() - 1;
  return static_cast<uint64_t>(round(cycles));
}

}  // namespace trace_processor
}  // namespace perfetto
