/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_TRACE_STORAGE_H_
#define SRC_TRACE_PROCESSOR_TRACE_STORAGE_H_

#include <array>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "perfetto/base/logging.h"

namespace perfetto {
namespace trace_processor {

// Stores a data inside a trace file in a columnar form. This makes it efficient
// to read or search across a single field of the trace (e.g. all the thread
// names for a given CPU).
class TraceStorage {
 public:
  constexpr static size_t kMaxCpus = 128;
  using StringId = size_t;

  class SlicesPerCpu {
   public:
    inline void AddSlice(uint64_t start_ns,
                         uint64_t duration_ns,
                         StringId thread_name_id) {
      start_ns_.emplace_back(start_ns);
      durations_.emplace_back(duration_ns);
      thread_names_.emplace_back(thread_name_id);
    }

    size_t slice_count() const {
      return start_ns_.size();
    }

    const std::deque<uint64_t>& start_ns() const {
      return start_ns_;
    }

    const std::deque<uint64_t>& durations() const {
      return durations_;
    }

   private:
    // Each vector below has the same number of entries (the number of slices
    // in the trace for the CPU).
    std::deque<uint64_t> start_ns_;
    std::deque<uint64_t> durations_;
    std::deque<StringId> thread_names_;
  };

  struct Stats {
    uint64_t mismatched_sched_switch_tids_ = 0;
  };

  virtual ~TraceStorage();

  // Adds a sched slice for a given cpu.
  // Virtual for testing.
  virtual void PushSchedSwitch(uint32_t cpu,
                               uint64_t timestamp,
                               uint32_t prev_pid,
                               uint32_t prev_state,
                               const char* prev_comm,
                               size_t prev_comm_len,
                               uint32_t next_pid);

  // Reading methods.
  const SlicesPerCpu& SlicesForCpu(uint32_t cpu) const {
    return cpu_events_[cpu];
  }

 private:
  // Each StringId is an offset into |strings_|.
  using StringHash = uint32_t;

  struct SchedSwitchEvent {
    uint64_t cpu = 0;
    uint64_t timestamp = 0;
    uint32_t prev_pid = 0;
    uint32_t prev_state = 0;
    StringId prev_thread_id = 0;
    uint32_t next_pid = 0;

    bool valid() const { return timestamp != 0; }
  };

  // Return an unqiue identifier for the contents of each string.
  // The string is copied internally and can be destroyed after this called.
  StringId InternString(const char* data, size_t length);

  // Metadata counters for events being added.
  Stats stats_;

  // One entry for each CPU in the trace.
  std::array<SchedSwitchEvent, kMaxCpus> last_sched_per_cpu_;

  // One entry for each CPU in the trace.
  std::array<SlicesPerCpu, kMaxCpus> cpu_events_;

  // One entry for each unique string in the trace.
  std::deque<std::string> string_pool_;

  // One entry for each unique string in the trace.
  std::unordered_map<StringHash, StringId> string_index_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TRACE_STORAGE_H_
