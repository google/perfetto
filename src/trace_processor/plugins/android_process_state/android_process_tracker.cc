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

#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

UniquePid AndroidProcessTracker::GetOrStartProcess(
    std::optional<int64_t> start_ts,
    int64_t pid,
    std::optional<int64_t> start_seq_id,
    base::StringView name) {
  ProcessTracker* process_tracker = context_->process_tracker.get();
  StringId name_id =
      name.empty() ? kNullStringId : context_->storage->InternString(name);

  // An incarnation we have already seen. Looking this up first matters: the
  // pid may since have been handed to a newer process, and a late event for
  // this one must not disturb that.
  if (start_seq_id.has_value()) {
    if (auto known = FindProcess(pid, *start_seq_id); known) {
      process_tracker->UpdateProcessName(*known, name_id,
                                         ProcessNamePriority::kSystem);
      if (start_ts) {
        process_tracker->SetStartTsIfUnset(*known, *start_ts);
      }
      return *known;
    }
  }

  if (auto live_upid = process_tracker->GetProcessOrNull(pid); live_upid) {
    // FindProcess() missed above, so if both seq ids are known they differ.
    bool recycled =
        GetStartSeqId(*live_upid).has_value() && start_seq_id.has_value();
    if (!recycled) {
      // Either this is the same incarnation, or we cannot prove otherwise.
      if (start_seq_id.has_value()) {
        RecordStartSeqId(*live_upid, *start_seq_id);
      }
      process_tracker->UpdateProcessName(*live_upid, name_id,
                                         ProcessNamePriority::kSystem);
      if (start_ts) {
        process_tracker->SetStartTsIfUnset(*live_upid, *start_ts);
      }
      return *live_upid;
    }
    // The pid has been recycled. StartNewProcess() below detaches it from the
    // previous incarnation and invalidates that process's threads, but leaves
    // it without an end_ts: we know it is gone, not when it went.
  }

  UniquePid upid =
      process_tracker->StartNewProcess(start_ts, std::nullopt, pid, name_id,
                                       ThreadNamePriority::kTrackDescriptor);
  if (start_seq_id.has_value()) {
    RecordStartSeqId(upid, *start_seq_id);
  }
  return upid;
}

std::optional<UniquePid> AndroidProcessTracker::EndProcess(
    int64_t ts,
    int64_t pid,
    int64_t start_seq_id) {
  std::optional<UniquePid> upid = FindProcess(pid, start_seq_id);
  if (!upid) {
    return std::nullopt;
  }
  ProcessTracker* process_tracker = context_->process_tracker.get();
  if (process_tracker->GetProcessOrNull(pid) == *upid) {
    process_tracker->EndThread(ts, pid);
    return upid;
  }
  auto process = (*context_->storage->mutable_process_table())[*upid];
  if (!process.end_ts().has_value()) {
    process.set_end_ts(ts);
  }
  // The pid was recycled, so EndThread() can't reach this process's main
  // thread: find it with a (rare) linear scan.
  std::optional<UniqueTid> main_utid;
  for (auto it = context_->storage->thread_table().IterateRows(); it; ++it) {
    if (it.upid() == *upid && it.tid() == pid && !it.end_ts().has_value()) {
      main_utid = it.id();
      break;
    }
  }
  if (main_utid) {
    (*context_->storage->mutable_thread_table())[*main_utid].set_end_ts(ts);
  }
  return upid;
}

std::optional<int64_t> AndroidProcessTracker::GetStartSeqId(
    UniquePid upid) const {
  const int64_t* start_seq_id = start_seq_id_by_upid_.Find(upid);
  return start_seq_id ? std::make_optional(*start_seq_id) : std::nullopt;
}

std::optional<UniquePid> AndroidProcessTracker::FindProcess(
    int64_t pid,
    int64_t start_seq_id) const {
  const UniquePid* upid = upid_by_seq_id_.Find(start_seq_id);
  if (!upid || context_->storage->process_table()[*upid].pid() != pid) {
    return std::nullopt;
  }
  return *upid;
}

void AndroidProcessTracker::RecordStartSeqId(UniquePid upid,
                                             int64_t start_seq_id) {
  start_seq_id_by_upid_[upid] = start_seq_id;
  upid_by_seq_id_[start_seq_id] = upid;
}

}  // namespace perfetto::trace_processor
