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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_ANDROID_PROCESS_STATE_ANDROID_PROCESS_STATE_MODULE_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_ANDROID_PROCESS_STATE_ANDROID_PROCESS_STATE_MODULE_H_

#include <cstdint>
#include <limits>
#include <map>
#include <optional>

#include "perfetto/protozero/field.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/importers/proto/track_event_extension_parser.h"
#include "src/trace_processor/plugins/android_process_state/tables_py.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto::trace_processor {

class TraceProcessorContext;

// Fills __intrinsic_android_process_state_change from the
// AndroidProcessStateChangedEvent deltas (one row each) and, at end of trace,
// synthesizes one initial-state row per process (is_initial = 1, ts NULL):
// processes that emitted a delta use the prev_* state of their earliest delta,
// the rest use their state from the trace-stop AndroidProcessState dump. Shared
// by the two importer hooks that feed it (the TrackEvent extension parser and
// the proto importer module).
class AndroidProcessStateTracker {
 public:
  AndroidProcessStateTracker(
      TraceProcessorContext* context,
      tables::AndroidProcessStateChangeTable* change_table);

  // A process_state_changed_event TrackEvent extension at |ts|.
  void ParseChange(int64_t ts, protozero::ConstBytes bytes);
  // An AndroidProcessState dump TracePacket.
  void ParseDump(protozero::ConstBytes bytes);
  // Synthesizes the per-process initial-state rows.
  void Finalize();

 private:
  // A process's raw (unresolved) state, from a dump entry or a delta's prev_*.
  struct ProcessStateValues {
    UniquePid upid = 0;
    int32_t pid = 0;
    std::optional<int32_t> uid;
    std::optional<int32_t> oom_score;
    std::optional<int32_t> proc_state;        // raw ProcessStateEnum value
    std::optional<int32_t> capability_flags;  // raw ProcessCapabilityEnum bits
  };

  // The prev_* state of a process's earliest delta, with that delta's ts.
  struct EarliestDelta {
    int64_t ts = std::numeric_limits<int64_t>::max();
    ProcessStateValues values;
  };

  void EmitInitialRow(const ProcessStateValues& v);

  StringId ProcStateName(int32_t value);
  StringId ReasonName(int32_t value);

  TraceProcessorContext* const context_;
  tables::AndroidProcessStateChangeTable* const change_table_;

  // Earliest delta's prev_* state per process (keyed by upid); overrides the
  // dump as the initial state of a process that changed.
  std::map<UniquePid, EarliestDelta> earliest_prev_;
  // Per-process state from the trace-stop dump, keyed by upid.
  std::map<UniquePid, ProcessStateValues> dump_;
};

// Receives the trace-stop AndroidProcessState dump (TracePacket field).
class AndroidProcessStateModule : public ProtoImporterModule {
 public:
  AndroidProcessStateModule(ProtoImporterModuleContext* module_context,
                            AndroidProcessStateTracker* tracker);
  ~AndroidProcessStateModule() override;

  void ParseField(const ParseFieldArgs& args) override;
  void OnEventsFullyExtracted() override;

 private:
  AndroidProcessStateTracker* const tracker_;
};

// Receives the per-process AndroidProcessStateChangedEvent deltas (TrackEvent
// extension field).
class AndroidProcessStateExtensionParser : public TrackEventExtensionParser {
 public:
  AndroidProcessStateExtensionParser(TrackEventExtensionParserContext* context,
                                     TraceProcessorContext* trace_context,
                                     AndroidProcessStateTracker* tracker);
  ~AndroidProcessStateExtensionParser() override;

  Result OnTrackEventSliceExtension(const TrackEventExtensionField& field,
                                    SliceId id) override;

 private:
  TraceProcessorContext* const trace_context_;
  AndroidProcessStateTracker* const tracker_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_ANDROID_PROCESS_STATE_ANDROID_PROCESS_STATE_MODULE_H_
