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

#include "src/trace_processor/trace_storage.h"

#include <string.h>

namespace perfetto {
namespace trace_processor {

TraceStorage::TraceStorage() {
  // Upid/utid 0 is reserved for invalid processes/threads.
  unique_processes_.emplace_back();
  unique_threads_.emplace_back();
}

TraceStorage::~TraceStorage() {}

void TraceStorage::PushSchedSwitch(uint32_t cpu,
                                   uint64_t timestamp,
                                   uint32_t prev_pid,
                                   uint32_t prev_state,
                                   const char* prev_comm,
                                   size_t prev_comm_len,
                                   uint32_t next_pid) {
  SchedSwitchEvent* prev = &last_sched_per_cpu_[cpu];

  // If we had a valid previous event, then inform the storage about the
  // slice.
  if (prev->valid() && prev->next_pid != 0 /* Idle process (swapper/N) */) {
    uint64_t duration = timestamp - prev->timestamp;
    cpu_events_[cpu].AddSlice(prev->timestamp, duration, prev->prev_pid,
                              prev->prev_thread_name_id);
  } else {
    cpu_events_[cpu].InitalizeSlices(this);
  }

  // If the this events previous pid does not match the previous event's next
  // pid, make a note of this.
  if (prev_pid != prev->next_pid) {
    stats_.mismatched_sched_switch_tids_++;
  }

  // Update the map with the current event.
  prev->cpu = cpu;
  prev->timestamp = timestamp;
  prev->prev_pid = prev_pid;
  prev->prev_state = prev_state;
  prev->prev_thread_name_id = InternString(prev_comm, prev_comm_len);
  prev->next_pid = next_pid;
}

void TraceStorage::PushProcess(uint32_t pid,
                               const char* process_name,
                               size_t process_name_len) {
  auto pids_pair = UpidsForPid(pid);
  auto proc_name_id = InternString(process_name, process_name_len);

  // We only create a new upid if there isn't one for that pid.
  if (pids_pair.first == pids_pair.second) {
    pids_.emplace(pid, unique_processes_.size());
    Process new_process;
    new_process.name_id = proc_name_id;
    unique_processes_.emplace_back(std::move(new_process));
  }
}

void TraceStorage::MatchThreadToProcess(uint32_t tid, uint32_t tgid) {
  auto tids_pair = UtidsForTid(tid);
  // We only care about tids for which we have a matching utid.
  PERFETTO_DCHECK(std::distance(tids_pair.first, tids_pair.second) <= 1);
  if (tids_pair.first != tids_pair.second) {
    PERFETTO_DCHECK(tids_pair.first->second < unique_threads_.size());
    Thread* thread = &unique_threads_[tids_pair.first->second];
    // If no upid is set - look it up.
    if (thread->upid == 0) {
      auto pids_pair = UpidsForPid(tgid);
      PERFETTO_DCHECK(std::distance(pids_pair.first, pids_pair.second) <= 1);
      if (pids_pair.first != pids_pair.second) {
        thread->upid = pids_pair.first->second;
        // If this is the first time we've used this process, set start_ns.
        Process* process = &unique_processes_[pids_pair.first->second];
        if (process->start_ns == 0)
          process->start_ns = thread->start_ns;
      }
    }
  }
}

TraceStorage::UniqueProcessRange TraceStorage::UpidsForPid(uint32_t pid) {
  return pids_.equal_range(pid);
}

TraceStorage::UniqueThreadRange TraceStorage::UtidsForTid(uint32_t tid) {
  return tids_.equal_range(tid);
}

TraceStorage::StringId TraceStorage::InternString(const char* data,
                                                  size_t length) {
  uint32_t hash = 0;
  for (size_t i = 0; i < length; ++i) {
    hash = static_cast<uint32_t>(data[i]) + (hash * 31);
  }
  auto id_it = string_index_.find(hash);
  if (id_it != string_index_.end()) {
    // TODO(lalitm): check if this DCHECK happens and if so, then change hash
    // to 64bit.
    PERFETTO_DCHECK(
        strncmp(string_pool_[id_it->second].c_str(), data, length) == 0);
    return id_it->second;
  }
  string_pool_.emplace_back(data, length);
  StringId string_id = string_pool_.size() - 1;
  string_index_.emplace(hash, string_id);
  return string_id;
}

}  // namespace trace_processor
}  // namespace perfetto
