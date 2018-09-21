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

#include <utility>

#include <inttypes.h>

namespace perfetto {
namespace trace_processor {

ProcessTracker::ProcessTracker(TraceProcessorContext* context)
    : context_(context) {
  // Create a mapping from (t|p)id 0 -> u(t|p)id 0 for the idle process.
  tids_.emplace(0, 0);
  pids_.emplace(0, 0);
}

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
    if (thread_name_id)
      thread->name_id = thread_name_id;
    return prev_utid;
  }

  // If none exist, assign a new utid and store it.
  UniqueTid new_utid = context_->storage->AddEmptyThread(tid);
  TraceStorage::Thread* thread = context_->storage->GetMutableThread(new_utid);
  thread->name_id = thread_name_id;
  if (timestamp)
    thread->start_ns = timestamp;
  tids_.emplace(tid, new_utid);
  return new_utid;
};

void ProcessTracker::UpdateThreadName(uint32_t tid,
                                      uint32_t pid,
                                      base::StringView name) {
  UniqueTid utid = UpdateThread(tid, pid);
  auto* thread = context_->storage->GetMutableThread(utid);
  auto name_id = context_->storage->InternString(name);
  thread->name_id = name_id;
}

UniqueTid ProcessTracker::UpdateThread(uint32_t tid, uint32_t pid) {
  auto tids_pair = tids_.equal_range(tid);

  // Try looking for a thread that matches both tid and thread group id (pid).
  TraceStorage::Thread* thread = nullptr;
  UniqueTid utid = 0;
  for (auto it = tids_pair.first; it != tids_pair.second; it++) {
    UniqueTid iter_utid = it->second;
    auto* iter_thread = context_->storage->GetMutableThread(iter_utid);
    if (iter_thread->upid == 0) {
      // We haven't discovered the parent process for the thread. Assign it
      // now and use this thread.
      thread = iter_thread;
      utid = iter_utid;
      break;
    }
    const auto& iter_process = context_->storage->GetProcess(iter_thread->upid);
    if (iter_process.pid == pid) {
      // We found a thread that matches both the tid and its parent pid.
      thread = iter_thread;
      utid = iter_utid;
      break;
    }
  }  // for(tids).

  // If no matching thread was found, create a new one.
  if (thread == nullptr) {
    utid = context_->storage->AddEmptyThread(tid);
    tids_.emplace(tid, utid);
    thread = context_->storage->GetMutableThread(utid);
  }

  // Find matching process or create new one.
  if (thread->upid == 0) {  // Not set, upid == 0 is invalid.
    std::tie(thread->upid, std::ignore) =
        GetOrCreateProcess(pid, thread->start_ns);
  }

  return utid;
}

UniquePid ProcessTracker::UpdateProcess(uint32_t pid, base::StringView name) {
  auto proc_name_id = context_->storage->InternString(name);
  UniquePid upid;
  TraceStorage::Process* process;
  std::tie(upid, process) = GetOrCreateProcess(pid, 0 /* start_ns */);
  process->name_id = proc_name_id;
  UpdateThread(/*tid=*/pid, pid);  // Create an entry for the main thread.
  return upid;
}

UniquePid ProcessTracker::UpdateProcess(uint32_t pid) {
  UniquePid upid;
  TraceStorage::Process* process;
  std::tie(upid, process) = GetOrCreateProcess(pid, 0 /* start_ns */);
  UpdateThread(/*tid=*/pid, pid);  // Create an entry for the main thread.
  return upid;
}

std::tuple<UniquePid, TraceStorage::Process*>
ProcessTracker::GetOrCreateProcess(uint32_t pid, uint64_t start_ns) {
  auto pids_pair = pids_.equal_range(pid);

  UniquePid upid = 0;
  for (auto it = pids_pair.first; it != pids_pair.second; it++) {
    if (it->first == pid) {
      upid = it->second;
      break;
    }
  }

  if (upid == 0) {
    upid = context_->storage->AddEmptyProcess(pid);
    pids_.emplace(pid, upid);
  }

  auto* process = context_->storage->GetMutableProcess(upid);
  if (process->start_ns == 0)
    process->start_ns = start_ns;

  // Give a default name to the process based on its PID just in case we never
  // get to see the real comm (e.g., we miss the trace packet that containts it
  // because of the ring buffer wrapping over).
  if (process->name_id == 0) {
    char process_name[64];
    size_t len =
        static_cast<size_t>(sprintf(process_name, "[pid:%" PRIu32 "]", pid));
    process->name_id =
        context_->storage->InternString(base::StringView(process_name, len));
  }

  return std::make_tuple(upid, process);
}

}  // namespace trace_processor
}  // namespace perfetto
