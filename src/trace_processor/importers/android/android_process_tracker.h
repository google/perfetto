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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_ANDROID_ANDROID_PROCESS_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_ANDROID_ANDROID_PROCESS_TRACKER_H_

#include <cstdint>
#include <map>
#include <optional>
#include <utility>

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto::trace_processor {

class TraceProcessorContext;

// Tracks Android process identity across pid reuse.
//
// Android recycles pids aggressively, so a pid on its own does not identify a
// process. The framework stamps every process incarnation with a "start
// sequence id" and repeats it on the death event and in the trace-stop
// android.process_state snapshot. This class uses that id to decide when a pid
// has been recycled, and to find the right process when a death event arrives
// after the pid has already been handed to a new one.
//
// All state here lives only for the duration of parsing. Importers that want
// to expose it at query time must copy it into a table of their own.
class AndroidProcessTracker {
 public:
  explicit AndroidProcessTracker(TraceProcessorContext* context)
      : context_(context) {}

  // Set when the Android framework is the authoritative source of process
  // lifecycle information: android.process_state is configured with
  // dump_process_metadata, and there is no ftrace supplying kernel-level
  // process events. Importers read this to decide whether to route process
  // lookups through this class.
  void SetFrameworkIsProcessAuthority(bool value) {
    framework_is_process_authority_ = value;
  }
  bool FrameworkIsProcessAuthority() const {
    return framework_is_process_authority_;
  }

  // Resolves the upid for an Android process.
  //
  // If |pid| is currently owned by a different incarnation (one whose start
  // seq id differs from |start_seq_id|), the pid is released from it and a new
  // process is started. The previous process is deliberately left without an
  // end_ts: a new process starting tells us the old one is gone, but not when
  // it died. That is recorded when the framework reports the death, which
  // locates its process via FindProcess().
  //
  // Note: android.util.proto.ProtoOutputStream omits zero-valued fields, so a
  // missing start_seq_id cannot be told apart from seq id 0. Records without
  // one are treated as belonging to the same incarnation.
  UniquePid GetOrStartProcess(std::optional<int64_t> start_ts,
                              int64_t pid,
                              std::optional<int64_t> start_seq_id,
                              StringId name,
                              ThreadNamePriority priority);

  // Returns the upid recorded for (|pid|, |start_seq_id|), if we have seen it.
  // Unlike ProcessTracker::GetProcessOrNull() this also finds incarnations
  // which no longer own the pid, which is what a death event arriving after a
  // recycle needs.
  std::optional<UniquePid> FindProcess(int64_t pid, int64_t start_seq_id) const;

  // Returns the start seq id of |upid|, if one has been seen.
  std::optional<int64_t> GetStartSeqId(UniquePid upid) const;

 private:
  void RecordStartSeqId(UniquePid upid, int64_t pid, int64_t start_seq_id);

  TraceProcessorContext* const context_;

  // (pid, start_seq_id) -> upid. Entries are kept after a pid is recycled so
  // that a late death event can still find the process it refers to.
  std::map<std::pair<int64_t, int64_t>, UniquePid> upid_by_pid_and_seq_id_;

  // upid -> start_seq_id.
  base::FlatHashMap<UniquePid, int64_t> start_seq_id_by_upid_;

  bool framework_is_process_authority_ = false;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_ANDROID_ANDROID_PROCESS_TRACKER_H_
