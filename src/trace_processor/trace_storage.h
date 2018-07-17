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

// Stores a data inside a trace file in a columnar form. This makes it efficient
// to read or search across a single field of the trace (e.g. all the thread
// names for a given CPU).
class TraceStorage {
 public:
  TraceStorage();

  constexpr static size_t kMaxCpus = 128;

  // StringId is an offset into |string_pool_|.
  using StringId = size_t;

  // UniquePid is an offset into |unique_processes_|. This is necessary because
  // Unix pids are reused and thus not guaranteed to be unique over a long
  // period of time.
  using UniquePid = uint32_t;
  using UniqueProcessIterator =
      std::multimap<uint32_t, UniquePid>::const_iterator;
  using UniqueProcessRange =
      std::pair<UniqueProcessIterator, UniqueProcessIterator>;

  // UniqueTid is an offset into |unique_threads_|. Necessary because tids can
  // be reused.
  using UniqueTid = uint32_t;
  using UniqueThreadIterator =
      std::multimap<uint32_t, UniqueTid>::const_iterator;
  using UniqueThreadRange =
      std::pair<UniqueThreadIterator, UniqueThreadIterator>;

  class SlicesPerCpu {
   public:
    inline void AddSlice(uint64_t start_ns,
                         uint64_t duration_ns,
                         uint32_t tid,
                         StringId thread_name_id) {
      start_ns_.emplace_back(start_ns);
      durations_.emplace_back(duration_ns);

      auto pair_it = storage_->tids_.equal_range(tid);
      // If there is a previous utid for that tid, use that.
      if (pair_it.first != pair_it.second) {
        UniqueTid prev_utid = std::prev(pair_it.second)->second;
        utids_.emplace_back(prev_utid);
      } else {
        // If none exist, assign a new utid and store it.
        Thread new_thread;
        new_thread.name_id = thread_name_id;
        new_thread.start_ns = start_ns;
        new_thread.upid = 0;
        storage_->tids_.emplace(tid, storage_->unique_threads_.size());
        utids_.emplace_back(storage_->unique_threads_.size());
        storage_->unique_threads_.emplace_back(std::move(new_thread));
      }
    }

    size_t slice_count() const { return start_ns_.size(); }

    const std::deque<uint64_t>& start_ns() const { return start_ns_; }

    const std::deque<uint64_t>& durations() const { return durations_; }

    const std::deque<UniqueTid>& utids() const { return utids_; }

    void InitalizeSlices(TraceStorage* storage) { storage_ = storage; }

   private:
    // Each vector below has the same number of entries (the number of slices
    // in the trace for the CPU).
    std::deque<uint64_t> start_ns_;
    std::deque<uint64_t> durations_;
    std::deque<UniqueTid> utids_;

    TraceStorage* storage_;
  };

  struct Stats {
    uint64_t mismatched_sched_switch_tids_ = 0;
  };

  virtual ~TraceStorage();

  // Information about a unique process seen in a trace.
  struct Process {
    uint64_t start_ns = 0;
    uint64_t end_ns = 0;
    StringId name_id;
  };

  // Information about a unique thread seen in a trace.
  struct Thread {
    uint64_t start_ns = 0;
    uint64_t end_ns = 0;
    StringId name_id;
    UniquePid upid;
  };

  // Adds a sched slice for a given cpu.
  // Virtual for testing.
  virtual void PushSchedSwitch(uint32_t cpu,
                               uint64_t timestamp,
                               uint32_t prev_pid,
                               uint32_t prev_state,
                               const char* prev_comm,
                               size_t prev_comm_len,
                               uint32_t next_pid);

  // Adds a process entry for a given pid.
  virtual void PushProcess(uint32_t pid,
                           const char* process_name,
                           size_t process_name_len);

  // Adds a thread entry for the tid.
  virtual void MatchThreadToProcess(uint32_t tid, uint32_t tgid);

  // Returns the bounds of a range that includes all UniquePids that have the
  // requested pid.
  UniqueProcessRange UpidsForPid(uint32_t pid);

  // Returns the bounds of a range that includes all UniqueTids that have the
  // requested tid.
  UniqueThreadRange UtidsForTid(uint32_t tid);

  // Reading methods.
  const SlicesPerCpu& SlicesForCpu(uint32_t cpu) const {
    PERFETTO_CHECK(cpu < cpu_events_.size());
    return cpu_events_[cpu];
  }

  const Process& GetProcess(UniquePid upid) const {
    PERFETTO_CHECK(upid < unique_processes_.size());
    return unique_processes_[upid];
  }

  const Thread& GetThread(UniqueTid utid) const {
    PERFETTO_CHECK(utid < unique_threads_.size());
    return unique_threads_[utid];
  }

  const std::string& GetString(StringId id) const {
    PERFETTO_CHECK(id < string_pool_.size());
    return string_pool_[id];
  }

  // |unique_processes_| always contains at least 1 element becuase the 0th ID
  // is reserved to indicate an invalid process.
  size_t process_count() const { return unique_processes_.size() - 1; }
  // |unique_threads_| always contains at least 1 element becuase the 0th ID
  // is reserved to indicate an invalid thread.
  size_t thread_count() const { return unique_threads_.size() - 1; }

 private:
  using StringHash = uint32_t;

  struct SchedSwitchEvent {
    uint64_t cpu = 0;
    uint64_t timestamp = 0;
    uint32_t prev_pid = 0;
    uint32_t prev_state = 0;
    StringId prev_thread_name_id = 0;
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

  // Each pid can have multiple UniquePid entries, a new UniquePid is assigned
  // each time a process is seen in the trace.
  std::multimap<uint32_t, UniquePid> pids_;

  // One entry for each UniquePid, with UniquePid as the index.
  std::deque<Process> unique_processes_;

  // Each tid can have multiple UniqueTid entries, a new UniqueTid is assigned
  // each time a thread is seen in the trace.
  std::multimap<uint32_t, UniqueTid> tids_;

  // One entry for each UniqueTid, with UniqueTid as the index.
  std::deque<Thread> unique_threads_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TRACE_STORAGE_H_
