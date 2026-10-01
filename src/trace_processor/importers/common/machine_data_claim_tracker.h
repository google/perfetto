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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_MACHINE_DATA_CLAIM_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_MACHINE_DATA_CLAIM_TRACKER_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

// Tracks per-time-window ownership of machine-wide data across multiple traces
// on the same machine.
//
// Rules:
//  1. Each trace contributes a single time window [min, max] per kind.
//     Non-overlapping spans are kept.
//  2. Conflicting/overlapping data from later traces is dropped and counted in
//     the per-kind machine_*_claim_conflict stat of that trace.
//  3. Open rows at window end (sched slice, thread state, shared track slices)
//     are closed if followed by another trace's window.
class MachineDataClaimTracker {
 public:
  enum class Kind : uint8_t {
    kSched = 0,
    kFtrace,
    kSysStats,
    kCpuPerUid,
    kAndroidPower,
    kCounter,
  };
  static constexpr size_t kKindCount = 6;

  explicit MachineDataClaimTracker(TraceProcessorContext* context);
  ~MachineDataClaimTracker();

  MachineDataClaimTracker(const MachineDataClaimTracker&) = delete;
  MachineDataClaimTracker& operator=(const MachineDataClaimTracker&) = delete;

  // Resolves the high-to-low precedence policy:
  // 1. Config tri-state (flag or RPC)
  // 2. Manifest merge_options.drop_duplicate_machine_data
  // 3. Default false (OFF)
  static bool IsEnabled(const TraceProcessorContext* context);

  static std::optional<Kind> KindForTracePacketField(uint32_t field_id);
  static const char* KindToString(Kind kind);
  static stats::KeyType StatForKind(Kind kind);

  static bool IsExclusive(Kind kind) { return kind == Kind::kCpuPerUid; }

  // Must be called at tokenization time for each piece of machine data of |kind|
  // at trace time |ts|.
  PERFETTO_ALWAYS_INLINE bool ShouldImport(TraceProcessorContext* context,
                                           Kind kind,
                                           int64_t ts) {
    if (PERFETTO_LIKELY(!IsEnabled(context))) {
      return true;
    }
    Claims& claims = claims_[static_cast<size_t>(kind)];
    if (PERFETTO_LIKELY(claims.last_accepted)) {
      Window& w = claims.windows[*claims.last_accepted];
      if (PERFETTO_LIKELY(w.trace_id == context->trace_id() && ts >= w.lo &&
                          ts <= w.hi)) {
        w.Extend(ts);
        return true;
      }
    }
    return ShouldImportSlow(context, kind, ts);
  }

  // Called when a machine-scoped track is created.
  void RegisterMachineTrack(TrackId track_id, std::string_view type);

  // Checks whether counter sample on |track| should be kept.
  static bool KeepCounter(TraceProcessorContext* caller,
                          TrackId track,
                          int64_t ts);

  // Called at the end of the import once all data has been parsed.
  void OnEventsFullyExtracted();

  // For testing
  struct WindowForTesting {
    TraceId trace_id;
    int64_t start;
    int64_t end;

    bool operator==(const WindowForTesting& o) const {
      return trace_id == o.trace_id && start == o.start && end == o.end;
    }
  };
  std::vector<WindowForTesting> GetWindowsForTesting(Kind kind) const;

 private:
  struct Window {
    void Extend(int64_t ts) {
      min = std::min(min, ts);
      max = std::max(max, ts);
    }
    void Freeze() {
      lo = min;
      hi = max;
    }

    TraceId trace_id;
    int64_t min;
    int64_t max;
    int64_t lo;
    int64_t hi;
  };

  struct Drops {
    TraceId trace_id;
    uint64_t count = 0;
  };

  struct Claims {
    std::vector<Window> windows;
    std::optional<size_t> last_accepted;
    std::vector<Drops> drops;
  };

  bool ShouldImportSlow(TraceProcessorContext* context, Kind kind, int64_t ts);
  void RecordDrop(TraceProcessorContext* context,
                  Kind kind,
                  int64_t ts,
                  std::optional<TraceId> conflicting_trace);

  const Window* FollowedWindowContaining(Kind kind, int64_t ts) const;
  std::optional<int64_t> CloseAtWindowEnd(Kind kind, int64_t ts);
  void CloseOpenSchedSlices();
  void CloseOpenThreadStates();
  void CloseOpenSlicesOnSharedTracks();

  TraceProcessorContext* const context_;
  const StringId kind_key_id_;
  const StringId conflicting_trace_id_key_id_;
  std::array<StringId, kKindCount> kind_names_;
  std::array<Claims, kKindCount> claims_;

  std::vector<bool> track_is_machine_;
  base::FlatHashMap<TrackId, Kind> shared_track_kinds_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_MACHINE_DATA_CLAIM_TRACKER_H_
