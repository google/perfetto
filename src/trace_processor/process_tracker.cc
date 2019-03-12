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
#include "src/trace_processor/stats.h"

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

UniqueTid ProcessTracker::StartNewThread(int64_t timestamp,
                                         uint32_t tid,
                                         StringId thread_name_id) {
  UniqueTid new_utid = context_->storage->AddEmptyThread(tid);
  TraceStorage::Thread* thread = context_->storage->GetMutableThread(new_utid);
  thread->name_id = thread_name_id;
  thread->start_ns = timestamp;
  tids_.emplace(tid, new_utid);
  return new_utid;
}

UniqueTid ProcessTracker::UpdateThread(int64_t timestamp,
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
  return StartNewThread(timestamp, tid, thread_name_id);
}

UniqueTid ProcessTracker::UpdateThread(uint32_t tid, uint32_t pid) {
  auto tids_pair = tids_.equal_range(tid);

  // Try looking for a thread that matches both tid and thread group id (pid).
  TraceStorage::Thread* thread = nullptr;
  UniqueTid utid = 0;
  for (auto it = tids_pair.first; it != tids_pair.second; it++) {
    UniqueTid iter_utid = it->second;
    auto* iter_thread = context_->storage->GetMutableThread(iter_utid);
    if (!iter_thread->upid.has_value()) {
      // We haven't discovered the parent process for the thread. Assign it
      // now and use this thread.
      thread = iter_thread;
      utid = iter_utid;
      break;
    }
    const auto& iter_process =
        context_->storage->GetProcess(iter_thread->upid.value());
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
  if (!thread->upid.has_value()) {
    std::tie(thread->upid, std::ignore) =
        GetOrCreateProcess(pid, thread->start_ns);
  }

  ResolvePendingAssociations(utid, *thread->upid);

  return utid;
}

UniquePid ProcessTracker::StartNewProcess(int64_t timestamp, uint32_t pid) {
  pids_.erase(pid);

  // Create a new UTID for the main thread, so we don't end up reusing an old
  // entry in case of TID recycling.
  StartNewThread(timestamp, /*tid=*/pid, 0);

  UniquePid upid = UpdateProcess(pid, timestamp);
  return upid;
}

UniquePid ProcessTracker::UpdateProcess(uint32_t pid,
                                        base::Optional<uint32_t> ppid,
                                        base::StringView name) {
  auto proc_name_id = context_->storage->InternString(name);

  base::Optional<UniquePid> pupid;
  if (ppid.has_value()) {
    pupid = GetOrCreateProcess(ppid.value(), 0 /* start_ns */).first;
  }
  UniquePid upid;
  TraceStorage::Process* process;
  std::tie(upid, process) = GetOrCreateProcess(pid, 0 /* start_ns */);
  process->name_id = proc_name_id;
  process->pupid = pupid;
  return upid;
}

UniquePid ProcessTracker::UpdateProcess(uint32_t pid, int64_t start_ns) {
  UniquePid upid;
  std::tie(upid, std::ignore) = GetOrCreateProcess(pid, start_ns);
  return upid;
}

std::pair<UniquePid, TraceStorage::Process*> ProcessTracker::GetOrCreateProcess(
    uint32_t pid,
    int64_t start_ns) {
  UniquePid upid;
  auto it = pids_.find(pid);
  if (it != pids_.end()) {
    upid = it->second;
  } else {
    upid = context_->storage->AddEmptyProcess(pid);
    pids_.emplace(pid, upid);

    // Create an entry for the main thread.
    // We cannot call StartNewThread() here, because threads for this process
    // (including the main thread) might have been seen already prior to this
    // call. This call usually comes from the ProcessTree dump which is delayed.
    UpdateThread(/*tid=*/pid, pid);
  }

  auto* process = context_->storage->GetMutableProcess(upid);
  if (process->start_ns == 0)
    process->start_ns = start_ns;

  return std::make_pair(upid, process);
}

void ProcessTracker::AssociateThreads(UniqueTid utid1, UniqueTid utid2) {
  TraceStorage::Thread* thd1 = context_->storage->GetMutableThread(utid1);
  TraceStorage::Thread* thd2 = context_->storage->GetMutableThread(utid2);

  // First of all check if one of the two threads is already bound to a process.
  // If that is the case, map the other thread to the same process and resolve
  // recursively any associations pending on the other thread.

  if (thd1->upid.has_value() && !thd2->upid.has_value()) {
    thd2->upid = *thd1->upid;
    ResolvePendingAssociations(utid2, *thd1->upid);
    return;
  }

  if (thd2->upid.has_value() && !thd1->upid.has_value()) {
    thd1->upid = *thd2->upid;
    ResolvePendingAssociations(utid1, *thd2->upid);
    return;
  }

  if (thd1->upid.has_value() && thd1->upid != thd2->upid) {
    // Cannot associate two threads that belong to two different processes.
    PERFETTO_ELOG("Process tracker failure. Cannot associate threads %u, %u",
                  thd1->tid, thd2->tid);
    context_->storage->IncrementStats(stats::process_tracker_errors);
    return;
  }

  pending_assocs_.emplace_back(utid1, utid2);
}

void ProcessTracker::ResolvePendingAssociations(UniqueTid utid_arg,
                                                UniquePid upid) {
  PERFETTO_DCHECK(context_->storage->GetMutableThread(utid_arg)->upid == upid);
  std::vector<UniqueTid> resolved_utids;
  resolved_utids.emplace_back(utid_arg);

  while (!resolved_utids.empty()) {
    UniqueTid utid = resolved_utids.back();
    resolved_utids.pop_back();
    for (auto it = pending_assocs_.begin(); it != pending_assocs_.end();) {
      UniqueTid other_utid;
      if (it->first == utid) {
        other_utid = it->second;
      } else if (it->second == utid) {
        other_utid = it->first;
      } else {
        ++it;
        continue;
      }

      PERFETTO_DCHECK(other_utid != utid);

      // Update the other thread and associated it to the same process.
      auto* other_thd = context_->storage->GetMutableThread(other_utid);
      PERFETTO_DCHECK(!other_thd->upid || other_thd->upid == upid);
      other_thd->upid = upid;

      // Erase the pair. The |pending_assocs_| vector is not sorted and swapping
      // a std::pair<uint32_t, uint32_t> is cheap.
      std::swap(*it, pending_assocs_.back());
      pending_assocs_.pop_back();

      // Recurse into the newly resolved thread. Some other threads might have
      // been bound to that.
      resolved_utids.emplace_back(other_utid);
    }
  }  // while (!resolved_utids.empty())
}

}  // namespace trace_processor
}  // namespace perfetto
