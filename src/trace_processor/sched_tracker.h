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

#ifndef SRC_TRACE_PROCESSOR_SCHED_TRACKER_H_
#define SRC_TRACE_PROCESSOR_SCHED_TRACKER_H_

#include <array>

#include "perfetto/base/string_view.h"
#include "perfetto/base/utils.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

// TODO:(b/111252261): The processing of cpu freq events and calculation
// of cycles is still to be implemented here.

// This class takes sched events from the trace and processes them to store
// as sched slices.
class SchedTracker {
 public:
  explicit SchedTracker(TraceProcessorContext*);
  SchedTracker(const SchedTracker&) = delete;
  SchedTracker& operator=(const SchedTracker&) = delete;
  virtual ~SchedTracker();

  struct SchedSwitchEvent {
    uint64_t timestamp = 0;
    uint32_t prev_pid = 0;
    uint32_t prev_state = 0;
    uint32_t next_pid = 0;

    bool valid() const { return timestamp != 0; }
  };

  // This method is called when a sched switch event is seen in the trace.
  virtual void PushSchedSwitch(uint32_t cpu,
                               uint64_t timestamp,
                               uint32_t prev_pid,
                               uint32_t prev_state,
                               base::StringView prev_comm,
                               uint32_t next_pid);

 private:
  // Store the previous sched event to calculate the duration before storing it.
  std::array<SchedSwitchEvent, base::kMaxCpus> last_sched_per_cpu_;

  TraceProcessorContext* const context_;
};
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SCHED_TRACKER_H_
