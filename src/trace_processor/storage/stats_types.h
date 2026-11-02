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

#ifndef SRC_TRACE_PROCESSOR_STORAGE_STATS_TYPES_H_
#define SRC_TRACE_PROCESSOR_STORAGE_STATS_TYPES_H_

#include <cstddef>

namespace perfetto::trace_processor::stats {

enum Type {
  kSingle,  // Single-value property, one value per key.
  kIndexed  // Indexed property, multiple value per key (e.g. cpu_stats[1]).
};

enum Severity {
  kInfo,      // Diagnostic counters
  kNotice,    // Normal but noteworthy condition worth surfacing to the user,
              // e.g. requested ftrace categories that failed to enable. Not a
              // bug and not data loss.
  kDataLoss,  // Correct operation that still resulted in data loss
  kError,     // If any kError counter is > 0 trace_processor_shell will
              // raise an error. This is also surfaced in the web UI.
  kNumSeverities,
};

enum Source {
  // The counter is collected when recording the trace on-device and is just
  // being reflected in the stats table.
  kTrace,

  // The counter is generated when importing / processing the trace in the trace
  // processor.
  kAnalysis,

  kNumSources,
};

// Scope of a stat defines its association within a potentially
// multi-machine/multi-trace merged session.
//
// Classification Reasoning:
// - kGlobal: Properties intrinsic to the TP process (e.g. parse timing).
// - kTrace: Properties of a recording session or trace file, not tied to a
//   specific machine (e.g. traced service stats, buffer stats).
// - kMachine: Static properties of a device. Currently unused for stats.
// - kMachineAndTrace: Per-machine execution data within a specific session
//   (e.g. ftrace stats, parser errors, clock sync).
#define PERFETTO_TP_STATS_SCOPES(F)                                 \
  F(kGlobal, "global"), F(kTrace, "trace"), F(kMachine, "machine"), \
      F(kMachineAndTrace, "machine_and_trace")

#define PERFETTO_TP_STATS_SCOPE_ENUM(varname, name) varname
enum class Scope : size_t {
  PERFETTO_TP_STATS_SCOPES(PERFETTO_TP_STATS_SCOPE_ENUM),
  kNumScopes,
};

#define PERFETTO_TP_STATS_SCOPE_NAME(varname, name) name
constexpr char const* kScopeNames[] = {
    PERFETTO_TP_STATS_SCOPES(PERFETTO_TP_STATS_SCOPE_NAME)};

}  // namespace perfetto::trace_processor::stats

#endif  // SRC_TRACE_PROCESSOR_STORAGE_STATS_TYPES_H_
