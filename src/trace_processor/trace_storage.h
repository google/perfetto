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
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "perfetto/base/logging.h"

namespace perfetto {
namespace trace_processor {

// UniquePid is an offset into |unique_processes_|. This is necessary because
// Unix pids are reused and thus not guaranteed to be unique over a long
// period of time.
using UniquePid = uint32_t;
// UniqueTid is an offset into |unique_threads_|. Necessary because tids can
// be reused.
using UniqueTid = uint32_t;

// Stores a data inside a trace file in a columnar form. This makes it efficient
// to read or search across a single field of the trace (e.g. all the thread
// names for a given CPU).
class TraceStorage {
 public:
  TraceStorage();
  TraceStorage(const TraceStorage&) = delete;

  virtual ~TraceStorage();

  constexpr static size_t kMaxCpus = 128;

  // StringId is an offset into |string_pool_|.
  using StringId = size_t;

  struct Stats {
    uint64_t mismatched_sched_switch_tids_ = 0;
  };

  // Information about a unique process seen in a trace.
  struct Process {
    uint64_t start_ns = 0;
    uint64_t end_ns = 0;
    StringId name_id = 0;
  };

  // Information about a unique thread seen in a trace.
  struct Thread {
    uint64_t start_ns = 0;
    uint64_t end_ns = 0;
    StringId name_id = 0;
    UniquePid upid = 0;
  };

  class SlicesPerCpu {
   public:
    inline void AddSlice(uint64_t start_ns,
                         uint64_t duration_ns,
                         UniqueTid utid) {
      start_ns_.emplace_back(start_ns);
      durations_.emplace_back(duration_ns);
      utids_.emplace_back(utid);
    }

    size_t slice_count() const { return start_ns_.size(); }

    const std::deque<uint64_t>& start_ns() const { return start_ns_; }

    const std::deque<uint64_t>& durations() const { return durations_; }

    const std::deque<UniqueTid>& utids() const { return utids_; }

   private:
    // Each vector below has the same number of entries (the number of slices
    // in the trace for the CPU).
    std::deque<uint64_t> start_ns_;
    std::deque<uint64_t> durations_;
    std::deque<UniqueTid> utids_;

  };

  void ResetStorage();

  void AddSliceToCpu(uint32_t cpu,
                     uint64_t start_ns,
                     uint64_t duration_ns,
                     UniqueTid utid);

  UniqueTid AddEmptyThread() {
    unique_threads_.emplace_back();
    return static_cast<UniqueTid>(unique_threads_.size() - 1);
  }

  UniquePid AddEmptyProcess() {
    unique_processes_.emplace_back();
    return static_cast<UniquePid>(unique_processes_.size() - 1);
  }

  void AddMismatchedSchedSwitch() { ++stats_.mismatched_sched_switch_tids_; }

  // Return an unqiue identifier for the contents of each string.
  // The string is copied internally and can be destroyed after this called.
  StringId InternString(const char* data, size_t length);

  Process* GetMutableProcess(UniquePid upid) {
    PERFETTO_DCHECK(upid < unique_processes_.size());
    return &unique_processes_[upid];
  }

  Thread* GetMutableThread(UniqueTid utid) {
    PERFETTO_DCHECK(utid < unique_threads_.size());
    return &unique_threads_[utid];
  }

  // Reading methods.
  const SlicesPerCpu& SlicesForCpu(uint32_t cpu) const {
    PERFETTO_DCHECK(cpu < cpu_events_.size());
    return cpu_events_[cpu];
  }

  const std::string& GetString(StringId id) const {
    PERFETTO_DCHECK(id < string_pool_.size());
    return string_pool_[id];
  }

  const Process& GetProcess(UniquePid upid) const {
    PERFETTO_DCHECK(upid < unique_processes_.size());
    return unique_processes_[upid];
  }

  const Thread& GetThread(UniqueTid utid) const {
    PERFETTO_DCHECK(utid < unique_threads_.size());
    return unique_threads_[utid];
  }

  // |unique_processes_| always contains at least 1 element becuase the 0th ID
  // is reserved to indicate an invalid process.
  size_t process_count() const { return unique_processes_.size() - 1; }
  // |unique_threads_| always contains at least 1 element becuase the 0th ID
  // is reserved to indicate an invalid thread.
  size_t thread_count() const { return unique_threads_.size() - 1; }

 private:
  TraceStorage& operator=(const TraceStorage&) = default;

  using StringHash = uint32_t;

  // Metadata counters for events being added.
  Stats stats_;

  // One entry for each CPU in the trace.
  std::array<SlicesPerCpu, kMaxCpus> cpu_events_;

  // One entry for each unique string in the trace.
  std::deque<std::string> string_pool_;

  // One entry for each unique string in the trace.
  std::unordered_map<StringHash, StringId> string_index_;

  // One entry for each UniquePid, with UniquePid as the index.
  std::deque<Process> unique_processes_;

  // One entry for each UniqueTid, with UniqueTid as the index.
  std::deque<Thread> unique_threads_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TRACE_STORAGE_H_
