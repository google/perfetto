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

#include "src/trace_processor/importers/android/android_process_tracker.h"

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

  if (auto live_upid = process_tracker->GetProcessOrNull(pid); live_upid) {
    std::optional<int64_t> live_seq_id = GetStartSeqId(*live_upid);
    bool recycled = live_seq_id.has_value() && start_seq_id.has_value() &&
                    *live_seq_id != *start_seq_id;
    if (!recycled) {
      if (start_seq_id.has_value() && !live_seq_id.has_value()) {
        RecordStartSeqId(*live_upid, pid, *start_seq_id);
      }
      process_tracker->UpdateProcessName(*live_upid, name,
                                         ProcessNamePriority::kSystem);
      return *live_upid;
    }
    // The pid has been recycled. Detach it from the previous incarnation so
    // that later lookups resolve to the new one, but leave that process
    // without an end_ts: we know it is gone, not when it went.
    // TODO: add a stat counting processes released this way which never
    // receive a death event, so the size of this gap is measurable.
    process_tracker->ReleasePid(*live_upid);
  }

  // We may already know this incarnation from an earlier event, having since
  // handed its pid to someone else. Reuse its upid rather than duplicating it.
  if (start_seq_id.has_value()) {
    if (auto known = FindProcess(pid, *start_seq_id); known) {
      process_tracker->UpdateProcessName(*known, name,
                                         ProcessNamePriority::kSystem);
      return *known;
    }
  }

  UniquePid upid = process_tracker->StartNewProcess(start_ts, std::nullopt, pid,
                                                    name, priority);
  if (start_seq_id.has_value()) {
    RecordStartSeqId(upid, pid, *start_seq_id);
  }
  return upid;
}

std::optional<UniquePid> AndroidProcessTracker::FindProcess(
    int64_t pid,
    int64_t start_seq_id) const {
  auto it = upid_by_pid_and_seq_id_.find(std::make_pair(pid, start_seq_id));
  if (it == upid_by_pid_and_seq_id_.end()) {
    return std::nullopt;
  }
  return it->second;
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
  upid_by_pid_and_seq_id_[std::make_pair(pid, start_seq_id)] = upid;
}

}  // namespace perfetto::trace_processor
