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
#include "perfetto/base/string_view.h"
#include "perfetto/base/utils.h"

namespace perfetto {
namespace trace_processor {

// UniquePid is an offset into |unique_processes_|. This is necessary because
// Unix pids are reused and thus not guaranteed to be unique over a long
// period of time.
using UniquePid = uint32_t;

// UniqueTid is an offset into |unique_threads_|. Necessary because tids can
// be reused.
using UniqueTid = uint32_t;

// StringId is an offset into |string_pool_|.
using StringId = size_t;

// Stores a data inside a trace file in a columnar form. This makes it efficient
// to read or search across a single field of the trace (e.g. all the thread
// names for a given CPU).
class TraceStorage {
 public:
  TraceStorage();
  TraceStorage(const TraceStorage&) = delete;

  virtual ~TraceStorage();

  struct Stats {
    uint64_t mismatched_sched_switch_tids_ = 0;
  };

  // Information about a unique process seen in a trace.
  struct Process {
    explicit Process(uint32_t p) : pid(p) {}
    uint64_t start_ns = 0;
    uint64_t end_ns = 0;
    StringId name_id = 0;
    uint32_t pid = 0;
  };

  // Information about a unique thread seen in a trace.
  struct Thread {
    explicit Thread(uint32_t t) : tid(t) {}
    uint64_t start_ns = 0;
    uint64_t end_ns = 0;
    StringId name_id = 0;
    UniquePid upid = 0;
    uint32_t tid = 0;
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

  class NestableSlices {
   public:
    inline void AddSlice(uint64_t start_ns,
                         uint64_t duration_ns,
                         UniqueTid utid,
                         StringId cat,
                         StringId name,
                         uint8_t depth,
                         uint64_t stack_id,
                         uint64_t parent_stack_id) {
      start_ns_.emplace_back(start_ns);
      durations_.emplace_back(duration_ns);
      utids_.emplace_back(utid);
      cats_.emplace_back(cat);
      names_.emplace_back(name);
      depths_.emplace_back(depth);
      stack_ids_.emplace_back(stack_id);
      parent_stack_ids_.emplace_back(parent_stack_id);
    }

    size_t slice_count() const { return start_ns_.size(); }
    const std::deque<uint64_t>& start_ns() const { return start_ns_; }
    const std::deque<uint64_t>& durations() const { return durations_; }
    const std::deque<UniqueTid>& utids() const { return utids_; }
    const std::deque<StringId>& cats() const { return cats_; }
    const std::deque<StringId>& names() const { return names_; }
    const std::deque<uint8_t>& depths() const { return depths_; }
    const std::deque<uint64_t>& stack_ids() const { return stack_ids_; }
    const std::deque<uint64_t>& parent_stack_ids() const {
      return parent_stack_ids_;
    }

   private:
    std::deque<uint64_t> start_ns_;
    std::deque<uint64_t> durations_;
    std::deque<UniqueTid> utids_;
    std::deque<StringId> cats_;
    std::deque<StringId> names_;
    std::deque<uint8_t> depths_;
    std::deque<uint64_t> stack_ids_;
    std::deque<uint64_t> parent_stack_ids_;
  };

  void ResetStorage();

  void AddSliceToCpu(uint32_t cpu,
                     uint64_t start_ns,
                     uint64_t duration_ns,
                     UniqueTid utid);

  UniqueTid AddEmptyThread(uint32_t tid) {
    unique_threads_.emplace_back(tid);
    return static_cast<UniqueTid>(unique_threads_.size() - 1);
  }

  UniquePid AddEmptyProcess(uint32_t pid) {
    unique_processes_.emplace_back(pid);
    return static_cast<UniquePid>(unique_processes_.size() - 1);
  }

  void AddMismatchedSchedSwitch() { ++stats_.mismatched_sched_switch_tids_; }

  // Return an unqiue identifier for the contents of each string.
  // The string is copied internally and can be destroyed after this called.
  StringId InternString(base::StringView);

  Process* GetMutableProcess(UniquePid upid) {
    PERFETTO_DCHECK(upid > 0 && upid < unique_processes_.size());
    return &unique_processes_[upid];
  }

  Thread* GetMutableThread(UniqueTid utid) {
    PERFETTO_DCHECK(utid > 0 && utid < unique_threads_.size());
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
    PERFETTO_DCHECK(upid > 0 && upid < unique_processes_.size());
    return unique_processes_[upid];
  }

  const Thread& GetThread(UniqueTid utid) const {
    PERFETTO_DCHECK(utid > 0 && utid < unique_threads_.size());
    return unique_threads_[utid];
  }

  const NestableSlices& nestable_slices() const { return nestable_slices_; }
  NestableSlices* mutable_nestable_slices() { return &nestable_slices_; }

  // |unique_processes_| always contains at least 1 element becuase the 0th ID
  // is reserved to indicate an invalid process.
  size_t process_count() const { return unique_processes_.size() - 1; }

  // |unique_threads_| always contains at least 1 element becuase the 0th ID
  // is reserved to indicate an invalid thread.
  size_t thread_count() const { return unique_threads_.size() - 1; }

  // Number of interned strings in the pool. Includes the empty string w/ ID=0.
  size_t string_count() const { return string_pool_.size(); }

 private:
  TraceStorage& operator=(const TraceStorage&) = default;

  using StringHash = uint64_t;

  // Metadata counters for events being added.
  Stats stats_;

  // One entry for each CPU in the trace.
  std::array<SlicesPerCpu, base::kMaxCpus> cpu_events_;

  // One entry for each unique string in the trace.
  std::deque<std::string> string_pool_;

  // One entry for each unique string in the trace.
  std::unordered_map<StringHash, StringId> string_index_;

  // One entry for each UniquePid, with UniquePid as the index.
  std::deque<Process> unique_processes_;

  // One entry for each UniqueTid, with UniqueTid as the index.
  std::deque<Thread> unique_threads_;

  // Slices coming from userspace events (e.g. Chromium TRACE_EVENT macros).
  NestableSlices nestable_slices_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TRACE_STORAGE_H_
