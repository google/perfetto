/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_processor/plugins/android_process_state/android_process_tracker.h"

#include <cstdint>
#include <optional>
#include <utility>

#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

UniquePid AndroidProcessTracker::GetOrStartProcess(
    std::optional<int64_t> start_ts,
    int64_t pid,
    std::optional<int64_t> start_seq_id,
    StringId name,
    ThreadNamePriority priority) {
  ProcessTracker* process_tracker = context_->process_tracker.get();

  // An incarnation we have already seen. Looking this up first matters: the
  // pid may since have been handed to a newer process, and a late event for
  // this one must not disturb that.
  if (start_seq_id.has_value()) {
    if (auto known = FindProcess(pid, *start_seq_id); known) {
      process_tracker->UpdateProcessName(*known, name,
                                         ProcessNamePriority::kSystem);
      return *known;
    }
  }

  if (auto live_upid = process_tracker->GetProcessOrNull(pid); live_upid) {
    std::optional<int64_t> live_seq_id = GetStartSeqId(*live_upid);
    bool recycled = live_seq_id.has_value() && start_seq_id.has_value() &&
                    *live_seq_id != *start_seq_id;
    if (!recycled) {
      // Either this is the same incarnation, or we cannot prove otherwise.
      if (start_seq_id.has_value() && !live_seq_id.has_value()) {
        RecordStartSeqId(*live_upid, pid, *start_seq_id);
      }
      process_tracker->UpdateProcessName(*live_upid, name,
                                         ProcessNamePriority::kSystem);
      return *live_upid;
    }
    // The pid has been recycled. StartNewProcess() below detaches it from the
    // previous incarnation and invalidates that process's threads, but leaves
    // it without an end_ts: we know it is gone, not when it went.
    // TODO: add a stat counting processes detached this way which never
    // receive a death event, so the size of this gap is measurable.
  }

  UniquePid upid = process_tracker->StartNewProcess(start_ts, std::nullopt, pid,
                                                    name, priority);
  RecordMainThread(upid, pid);
  if (start_seq_id.has_value()) {
    RecordStartSeqId(upid, pid, *start_seq_id);
  }
  return upid;
}

void AndroidProcessTracker::EndProcess(int64_t ts, UniquePid upid) {
  ProcessTracker* process_tracker = context_->process_tracker.get();
  auto process = (*context_->storage->mutable_process_table())[upid];
  auto& thread_table = *context_->storage->mutable_thread_table();

  // If the process still owns its pid, ProcessTracker can do the whole job:
  // EndThread() ends the main thread, the process itself, and frees the pid.
  // Check the live thread really belongs to |upid|, because once the pid has
  // been recycled it belongs to the successor and must not be touched.
  if (auto utid = process_tracker->GetThreadOrNull(process.pid()); utid) {
    if (thread_table[*utid].upid() == upid) {
      process_tracker->EndThread(ts, process.pid());
      return;
    }
  }

  // The pid has been handed on, so there is no way to reach this process
  // through ProcessTracker any more: StartNewProcess() erased its pid and tid
  // mappings and invalidated its threads when the successor started. Nothing
  // in the tracker still refers to |upid|, so closing the rows here cannot
  // desynchronise it. Without this the process would stay open-ended, and
  // queries which clip by process.end_ts (e.g. android.memory.breakdown)
  // would keep attributing its data for the rest of the trace.
  if (auto* utid = main_utid_by_upid_.Find(upid); utid) {
    if (auto thread = thread_table[*utid]; !thread.end_ts().has_value()) {
      thread.set_end_ts(ts);
    }
  }
  if (!process.end_ts().has_value()) {
    process.set_end_ts(ts);
  }
}

std::optional<UniquePid> AndroidProcessTracker::FindProcess(
    int64_t pid,
    int64_t start_seq_id) const {
  const UniquePid* upid = upid_by_pid_and_seq_id_.Find({pid, start_seq_id});
  return upid ? std::make_optional(*upid) : std::nullopt;
}

std::optional<int64_t> AndroidProcessTracker::GetStartSeqId(
    UniquePid upid) const {
  const int64_t* start_seq_id = start_seq_id_by_upid_.Find(upid);
  return start_seq_id ? std::make_optional(*start_seq_id) : std::nullopt;
}

void AndroidProcessTracker::RecordStartSeqId(UniquePid upid,
                                             int64_t pid,
                                             int64_t start_seq_id) {
  start_seq_id_by_upid_[upid] = start_seq_id;
  upid_by_pid_and_seq_id_[{pid, start_seq_id}] = upid;
}

void AndroidProcessTracker::RecordMainThread(UniquePid upid, int64_t pid) {
  // StartNewProcess() has just created this thread, so the pid still resolves
  // to it. Remember it now: after the pid is recycled there is no way back.
  if (auto utid = context_->process_tracker->GetThreadOrNull(pid); utid) {
    main_utid_by_upid_[upid] = *utid;
  }
}

}  // namespace perfetto::trace_processor
