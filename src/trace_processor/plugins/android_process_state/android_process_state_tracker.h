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

#include "perfetto/protozero/field.h"
#include "src/trace_processor/plugins/android_process_state/tables_py.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/util/descriptors.h"

namespace perfetto::trace_processor {
class TraceProcessorContext;
}  // namespace perfetto::trace_processor

namespace perfetto::trace_processor::android_process_state {

StringId InternEnum(TraceProcessorContext* context,
                    DescriptorPool::CachedDescriptor& cache,
                    const char* enum_name,
                    int32_t value);

// Tracks process states and freezer states from incremental track events and
// trace-stop dump snapshots, and synthesizes per-process initial-state rows
// (is_initial = 1, ts = NULL) at finalization.
class AndroidProcessStateTracker {
 public:
  AndroidProcessStateTracker(
      TraceProcessorContext* context,
      tables::AndroidProcessStateTable* process_state_table,
      tables::AndroidFreezerStateTable* freezer_state_table);

  // A process_state_changed_event TrackEvent extension at |ts|.
  void ParseProcessStateChange(int64_t ts, protozero::ConstBytes bytes);
  // An AndroidProcessState dump TracePacket.
  void ParseProcessStateDump(protozero::ConstBytes bytes);

  // An AndroidFreezerEvent TrackEvent extension at |ts|.
  void ParseFreezerEvent(int64_t ts, protozero::ConstBytes bytes);
  // An AndroidFreezerState dump TracePacket.
  void ParseFreezerDump(protozero::ConstBytes bytes);

  // Synthesizes the per-process initial-state rows.
  void Finalize();

 private:
  // Information captured for a process from dumps or delta events.
  struct ProcessStateValues {
    UniquePid upid = 0;
    std::optional<int32_t> oom_score;
    std::optional<int32_t> proc_state;
    std::optional<int32_t> capability_flags;
  };

  // Information captured for freezer state from trace-stop dumps.
  struct FreezerStateValues {
    UniquePid upid = 0;
    std::optional<int32_t> unfreeze_reason;
  };

  // Tracks the state before the first observed change (prev_*) for a process,
  // used to reconstruct its baseline state at trace start.
  struct EarliestDelta {
    int64_t ts = std::numeric_limits<int64_t>::max();
    ProcessStateValues values;
  };

  // Records the pre-change state (prev_*) from the earliest delta track event
  // seen for a process to establish its baseline at trace start.
  void UpdateInitialStateFromDelta(int64_t ts,
                                   const ProcessStateValues& prev_state);

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

  TraceProcessorContext* const context_;
  tables::AndroidProcessStateTable* const process_state_table_;
  tables::AndroidFreezerStateTable* const freezer_state_table_;

  DescriptorPool::CachedDescriptor proc_state_cache_;
  DescriptorPool::CachedDescriptor reason_cache_;
  DescriptorPool::CachedDescriptor unfreeze_reason_cache_;

  // Map of upid -> earliest observed delta transition during the trace.
  std::map<UniquePid, EarliestDelta> earliest_prev_;
  // Map of upid -> final process state from the trace-stop dump snapshot.
  std::map<UniquePid, ProcessStateValues> process_dump_;
  // Map of upid -> final freezer state from the trace-stop dump snapshot.
  std::map<UniquePid, FreezerStateValues> freezer_dump_;
};

}  // namespace perfetto::trace_processor::android_process_state

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_ANDROID_PROCESS_STATE_ANDROID_PROCESS_STATE_TRACKER_H_
