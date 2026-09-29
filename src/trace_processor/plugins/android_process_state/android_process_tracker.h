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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_ANDROID_PROCESS_STATE_ANDROID_PROCESS_TRACKER_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_ANDROID_PROCESS_STATE_ANDROID_PROCESS_TRACKER_H_

#include <cstdint>
#include <optional>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto::trace_processor {

class TraceProcessorContext;

// Tracks Android process identity across pid reuse.
//
// Android recycles pids aggressively, so a pid on its own does not identify a
// process. The framework stamps every process incarnation with a monotonic
// "start sequence id" and repeats it on the death event and in the
// android.process_state snapshots. This class uses that id to decide when a pid
// has been recycled, and to find the right process when a death event arrives
// after the pid has already been handed to a new one.
//
// All state here lives only for the duration of parsing. Importers that want
// to expose it at query time must copy it into a table of their own.
class AndroidProcessTracker {
 public:
  explicit AndroidProcessTracker(TraceProcessorContext* context)
      : context_(context) {}

  // Set when an AndroidProcessState dump shows dump_process_metadata is on.
  void SetFrameworkIsProcessAuthority() {
    framework_is_process_authority_ = true;
  }
  bool FrameworkIsProcessAuthority() const {
    return framework_is_process_authority_;
  }

  // Resolves the upid for an Android process.
  //
  // If |pid| is currently owned by a different incarnation (one whose start
  // seq id differs from |start_seq_id|), a new process is started.
  // ProcessTracker::StartNewProcess() detaches the pid from the previous
  // incarnation on our behalf, and deliberately leaves it without an end_ts:
  // a new process starting tells us the old one is gone, but not when it
  // died. That is recorded by EndProcess().
  //
  // Note: android.util.proto.ProtoOutputStream omits zero-valued fields, so a
  // missing start_seq_id cannot be told apart from seq id 0. Records without
  // one are treated as belonging to the same incarnation. An empty |name|
  // means unknown.
  UniquePid GetOrStartProcess(std::optional<int64_t> start_ts,
                              int64_t pid,
                              std::optional<int64_t> start_seq_id,
                              base::StringView name);

  // Ends the (|pid|, |start_seq_id|) incarnation at |ts|, even if its pid has
  // since been recycled. Returns its upid, or nullopt if it was never seen.
  std::optional<UniquePid> EndProcess(int64_t ts,
                                      int64_t pid,
                                      int64_t start_seq_id);

  // Returns the start seq id of |upid|, if one has been seen.
  std::optional<int64_t> GetStartSeqId(UniquePid upid) const;

 private:
  // Returns the upid recorded for (|pid|, |start_seq_id|), if we have seen it.
  // Unlike ProcessTracker::GetProcessOrNull() this also finds incarnations
  // which no longer own the pid, which is what a death event arriving after a
  // recycle needs.
  std::optional<UniquePid> FindProcess(int64_t pid, int64_t start_seq_id) const;

  void RecordStartSeqId(UniquePid upid, int64_t start_seq_id);

  TraceProcessorContext* const context_;

  // start_seq_id -> upid. Entries are kept after a pid is recycled so that a
  // late death event can still find the process it refers to.
  base::FlatHashMap<int64_t, UniquePid> upid_by_seq_id_;

  // upid -> start_seq_id.
  base::FlatHashMap<UniquePid, int64_t> start_seq_id_by_upid_;

  bool framework_is_process_authority_ = false;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_ANDROID_PROCESS_STATE_ANDROID_PROCESS_TRACKER_H_
