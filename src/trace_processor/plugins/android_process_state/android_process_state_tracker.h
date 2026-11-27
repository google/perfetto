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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_ANDROID_PROCESS_STATE_ANDROID_PROCESS_STATE_TRACKER_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_ANDROID_PROCESS_STATE_ANDROID_PROCESS_STATE_TRACKER_H_

#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

#include "perfetto/protozero/field.h"
#include "src/trace_processor/importers/android/unknown_process_logger.h"
#include "src/trace_processor/plugins/android_process_state/tables_py.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/util/descriptors.h"

namespace perfetto::trace_processor {
class ArgsInserter;
class TraceProcessorContext;
}  // namespace perfetto::trace_processor

namespace perfetto::trace_processor::android_process_state {

StringId InternEnum(TraceProcessorContext* context,
                    DescriptorPool::CachedDescriptor& cache,
                    const char* enum_name,
                    int32_t value);

// Tracks process states and freezer states from incremental track events and
// trace-stop dump snapshots, and synthesizes per-process initial-state rows
// (is_initial = 1, ts = NULL) at finalization. Initial process state rows carry
// the time the state was entered, when known, in state_start_ts. Also records
// the process groups requested through Process.setProcessGroup.
class AndroidProcessStateTracker {
 public:
  AndroidProcessStateTracker(
      TraceProcessorContext* context,
      tables::AndroidProcessStateTable* process_state_table,
      tables::AndroidFreezerStateTable* freezer_state_table,
      tables::AndroidProcessGroupTable* process_group_table);

  // A process_state_changed_event TrackEvent extension at |ts|.
  void ParseProcessStateChange(int64_t ts, protozero::ConstBytes bytes);
  // A process_state_died_event TrackEvent extension at |ts|.
  void ParseProcessStateDied(int64_t ts, protozero::ConstBytes bytes);
  // An AndroidProcessState dump TracePacket at |ts|.
  void ParseProcessStateDump(int64_t ts, protozero::ConstBytes bytes);

  // An AndroidFreezerEvent TrackEvent extension at |ts|.
  void ParseFreezerEvent(int64_t ts, protozero::ConstBytes bytes);
  // An AndroidFreezerState dump TracePacket at |ts|.
  void ParseFreezerDump(int64_t ts, protozero::ConstBytes bytes);

  // An AndroidProcessGroupChangedEvent TrackEvent extension at |ts|. Inserts
  // one row per event. Unlike the other tables, no initial rows are
  // synthesized: the event carries no previous group, and the snapshot's
  // process_group is OomAdjuster's view, not the group last requested through
  // Process.setProcessGroup, so the group before a process's first event is
  // unknown.
  void ParseProcessGroupChange(int64_t ts, protozero::ConstBytes bytes);

  // Synthesizes the per-process initial-state rows.
  void Finalize();

 private:
  // Returns the live process for |pid|, or std::nullopt if |pid| is <= 0 (also
  // when it is missing from the event, as it then decodes as 0) or has no live
  // process. In the latter case, records an android_track_event_process_unknown
  // notice for |source|, with |process_name| and |start_seq_id| when known.
  std::optional<UniquePid> ResolveLiveProcess(
      int64_t ts,
      const char* source,
      int64_t pid,
      protozero::ConstChars process_name = {},
      std::optional<int64_t> start_seq_id = std::nullopt);

  // Information captured for a process from dumps or delta events.
  struct ProcessStateValues {
    UniquePid upid = 0;
    std::optional<int32_t> oom_score;
    std::optional<int32_t> proc_state;
    std::optional<int32_t> capability_flags;
    std::optional<int32_t> process_group;
    // Trace time at which the process entered |proc_state|, converted from
    // the CLOCK_BOOTTIME proc_state_start_ns / prev_proc_state_start_ns.
    std::optional<int64_t> start_ts;
  };

  // Information captured for freezer state from trace-stop dumps.
  struct FreezerStateValues {
    UniquePid upid = 0;
    std::optional<int32_t> unfreeze_reason;
    // Trace time at which the process was last unfrozen / frozen.
    std::optional<int64_t> unfrozen_start_ts;
    std::optional<int64_t> frozen_start_ts;
  };

  // The track-event stream only emits delta transitions (prev -> cur). To
  // reconstruct each process's state at the start of the trace, records the
  // prev_* fields shared by AndroidProcessStateChangedEvent and
  // AndroidProcessStateDiedEvent from the earliest such event of |upid|, at
  // |ts|. Later events return before reading or validating them. Defined in the
  // .cc file, the only place it is instantiated.
  template <typename Decoder>
  void UpdateInitialStateFromDelta(int64_t ts,
                                   UniquePid upid,
                                   const char* source,
                                   const Decoder& event);

  // Converts a CLOCK_BOOTTIME start time written by system_server (e.g.
  // proc_state_start_ns, frozen_start_ns) to trace time. Returns std::nullopt
  // if |boottime_ns| is 0 (unknown), conversion fails (the clock tracker
  // records a stat), |boottime_ns| is negative, or the converted time is after
  // |event_ts|. The last two cases are invalid and recorded via the
  // android_process_state_invalid_start_ts stat and an import log.
  std::optional<int64_t> StartNsToTraceTime(int64_t event_ts,
                                            const char* source,
                                            const char* field,
                                            UniquePid upid,
                                            int64_t boottime_ns);

  // Tracks the state before the first observed change (prev_*) for a process,
  // used to reconstruct its baseline state at trace start.
  struct EarliestDelta {
    int64_t ts = std::numeric_limits<int64_t>::max();
    ProcessStateValues values;
  };

  // Computes the initial baseline state for every process by starting from the
  // trace-stop dump snapshot and overlaying the pre-transition states from the
  // earliest observed deltas.
  std::map<UniquePid, ProcessStateValues> ComputeInitialProcessStates() const;

  // Emits a synthesized initial baseline row (is_initial = 1, ts = NULL) into
  // the AndroidProcessState table.
  void EmitInitialProcessStateRow(const ProcessStateValues& v);

  // Emits a synthesized initial baseline row (is_initial = 1, ts = NULL) into
  // the AndroidFreezerState table.
  void EmitInitialFreezerRow(const FreezerStateValues& v);

  // Populates decoded flag array entries (capability_flags[i],
  // cpu_time_reasons[i], implicit_cpu_time_reasons[i], freeze_policy_flags[i])
  // onto row.arg_set_id using a single ArgsInserter.
  void AddFlagsArgs(tables::AndroidProcessStateTable::Id id,
                    const tables::AndroidProcessStateTable::Row& row);

  // Appends decoded flag entries for a single non-zero bitmask onto |inserter|.
  void EmitDecodedFlags(ArgsInserter& inserter,
                        StringId flat_key_id,
                        const char* key_prefix,
                        DescriptorPool::CachedDescriptor& cache,
                        const char* enum_name,
                        int32_t mask);

  TraceProcessorContext* const context_;
  tables::AndroidProcessStateTable* const process_state_table_;
  tables::AndroidFreezerStateTable* const freezer_state_table_;
  tables::AndroidProcessGroupTable* const process_group_table_;
  const StringId capability_flags_id_;
  const StringId cpu_time_reasons_id_;
  const StringId implicit_cpu_time_reasons_id_;
  const StringId freeze_policy_flags_id_;
  UnknownProcessLogger unknown_process_logger_;

  DescriptorPool::CachedDescriptor proc_state_cache_;
  DescriptorPool::CachedDescriptor process_group_cache_;
  DescriptorPool::CachedDescriptor reason_cache_;
  DescriptorPool::CachedDescriptor unfreeze_reason_cache_;
  DescriptorPool::CachedDescriptor capability_flags_cache_;
  DescriptorPool::CachedDescriptor cpu_time_reasons_cache_;
  DescriptorPool::CachedDescriptor freeze_policy_flags_cache_;
  std::vector<std::string_view> flag_views_;

  // Map of upid -> earliest observed delta transition during the trace.
  std::map<UniquePid, EarliestDelta> earliest_prev_;
  // Map of upid -> final process state from the trace-stop dump snapshot.
  std::map<UniquePid, ProcessStateValues> process_dump_;
  // Map of upid -> final freezer state from the trace-stop dump snapshot.
  std::map<UniquePid, FreezerStateValues> freezer_dump_;
};

}  // namespace perfetto::trace_processor::android_process_state

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_ANDROID_PROCESS_STATE_ANDROID_PROCESS_STATE_TRACKER_H_
