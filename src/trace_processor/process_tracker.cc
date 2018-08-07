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

#include "src/trace_processor/process_tracker.h"

namespace perfetto {
namespace trace_processor {

ProcessTracker::ProcessTracker(TraceProcessorContext* context)
    : context_(context){};

ProcessTracker::~ProcessTracker() = default;

UniqueTid ProcessTracker::UpdateThread(uint64_t timestamp,
                                       uint32_t tid,
                                       StringId thread_name_id) {
  auto pair_it = tids_.equal_range(tid);

  // If a utid exists for the tid, find it and update the name.
  if (pair_it.first != pair_it.second) {
    auto prev_utid = std::prev(pair_it.second)->second;
    TraceStorage::Thread* thread =
        context_->storage->GetMutableThread(prev_utid);
    thread->name_id = thread_name_id;
    return prev_utid;
  }

  // If none exist, assign a new utid and store it.
  UniqueTid new_utid = context_->storage->AddEmptyThread();
  TraceStorage::Thread* thread = context_->storage->GetMutableThread(new_utid);
  thread->name_id = thread_name_id;
  thread->start_ns = timestamp;
  tids_.emplace(tid, new_utid);
  return new_utid;
};

UniqueTid ProcessTracker::UpdateThread(uint32_t tid, uint32_t tgid) {
  auto tids_pair = tids_.equal_range(tid);

  // TODO(b/110409911): Remove once invalidation of threads is implemented.
  PERFETTO_DCHECK(std::distance(tids_pair.first, tids_pair.second) <= 1);

  UniqueTid utid = 0;
  // Find matching thread for tid or create new one.
  TraceStorage::Thread* thread = nullptr;
  if (tids_pair.first == tids_pair.second) {
    utid = context_->storage->AddEmptyThread();
    tids_.emplace(tid, utid);
    thread = context_->storage->GetMutableThread(utid);
  } else {
    utid = tids_pair.first->second;
    thread = context_->storage->GetMutableThread(utid);
  }

  // Find matching upid for tgid or create new one.
  if (thread->upid == 0) {
    auto pids_pair = pids_.equal_range(tgid);

    // TODO(b/110409911): Remove once invalidation of threads is implemented.
    PERFETTO_DCHECK(std::distance(pids_pair.first, pids_pair.second) <= 1);

    TraceStorage::Process* process = nullptr;
    if (pids_pair.first == pids_pair.second) {
      UniquePid new_upid = context_->storage->AddEmptyProcess();
      pids_.emplace(tgid, new_upid);
      process = context_->storage->GetMutableProcess(new_upid);
      thread->upid = new_upid;
    } else {
      process = context_->storage->GetMutableProcess(pids_pair.first->second);
      thread->upid = pids_pair.first->second;
    }
    if (process->start_ns == 0)
      process->start_ns = thread->start_ns;
  }
  return utid;
}

UniquePid ProcessTracker::UpdateProcess(uint32_t pid,
                                        const char* process_name,
                                        size_t process_name_len) {
  auto pids_pair = pids_.equal_range(pid);
  auto proc_name_id =
      context_->storage->InternString(process_name, process_name_len);

  // If a upid exists for the pid, find it and update the name.
  if (pids_pair.first != pids_pair.second) {
    auto prev_upid = std::prev(pids_pair.second)->second;
    TraceStorage::Process* process =
        context_->storage->GetMutableProcess(prev_upid);
    process->name_id = proc_name_id;
    return prev_upid;
  }

  // Create a new upid if there isn't one for that pid.
  UniquePid new_upid = context_->storage->AddEmptyProcess();
  TraceStorage::Process* process =
      context_->storage->GetMutableProcess(new_upid);
  pids_.emplace(pid, new_upid);
  process->name_id = proc_name_id;
  return new_upid;
}

}  // namespace trace_processor
}  // namespace perfetto
