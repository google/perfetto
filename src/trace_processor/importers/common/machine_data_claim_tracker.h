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
#include <limits>
#include <optional>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/hash.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

// Arbitrates machine-wide data between the traces of a merged input which
// were recorded on the same machine.
//
// Some data describes state which exists exactly once per machine: the kernel
// scheduler, per-CPU/GPU frequency and idle state, system-wide counters,
// battery and power rails. Every trace which recorded such data describes the
// same physical entities, so two traces must never provide the same kind of
// data for the same stretch of time: there is no sensible way to interpret
// two overlapping scheduling timelines for one CPU, or two frequency tracks
// for one CPU.
//
// The rules, per (machine, kind of data):
//  1. Each trace may contribute data of a kind for a single contiguous time
//     window, and windows of different traces never overlap. The first trace
//     (in trace_file order) to provide a kind gets all of its data; a later
//     trace's data is kept only where it does not overlap the windows of the
//     traces before it. The common case of traces recorded one after another
//     is lossless.
//  2. Everything else is dropped, counted in the
//     `machine_data_claimed_by_other_trace` stat of the dropping (machine,
//     trace) and explained once per (trace, kind) in the trace_import_logs
//     table.
//  3. Machine-wide tracks (i.e. those without a thread or process dimension)
//     of claimed data are shared by all traces on the machine: two
//     non-overlapping traces recording CPU frequency produce one
//     cpu_frequency track per CPU, not two. See TrackTracker.
//  4. At the end of the import, state left open at the end of a window (the
//     last sched slice of each CPU, the last thread state of each thread,
//     unfinished slices on shared tracks) is closed at the end of that window
//     if a later window of another trace follows it. This stops it
//     extending, as it would for the last trace, until the end of the whole
//     merged trace, over the next trace's data. Each such row is counted in
//     the machine_data_closed_at_trace_boundary stat of its trace, as its
//     true end is unknown.
//
// Some kinds (see IsExclusive) keep per-trace state which can't be stitched
// across traces; for those the first trace owns the kind for all time.
//
// Data which is not machine-wide (e.g. track events, process metadata,
// profiles) is unaffected, so merging different data sources recorded on one
// machine at the same time (e.g. a system trace and an app's SDK trace) is
// unchanged.
//
// Windows are decided at tokenization time, on trace time timestamps, i.e.
// after clock conversion. This relies on traces of a merged input being
// tokenized one after the other, so that when a trace starts providing a
// kind, the windows of the traces before it are final. Traces attributed to
// the same machine are assumed to share a boot (see ClockTracker): traces
// from different boots of a device must be attributed to different machines
// with a perfetto_manifest.
//
// One instance exists per machine and is shared by all traces on it.
class MachineDataClaimTracker {
 public:
  // The kinds of machine-wide data. Data of different kinds is arbitrated
  // independently, so kinds are split by data source: a system trace with
  // ftrace and another trace with only sys_stats recorded at the same time
  // both keep their data. Sources which write the same machine state (e.g.
  // every source of scheduling data) must share a kind.
  enum class Kind : uint8_t {
    // The kernel's view of the machine: scheduling, thread states, per-CPU
    // and per-GPU frequency/idle and every other ftrace event. Sources:
    // Linux ftrace (proto, systrace text and JSON systemTraceEvents), ETW and
    // generic kernel events.
    kKernel = 0,
    // The linux.sys_stats data source.
    kSysStats,
    // Android per-UID CPU time (cpu_per_uid_data).
    kCpuPerUid,
    // Android power data: battery counters, power rails, entity state
    // residency and energy estimation breakdowns.
    kAndroidPower,
  };
  static constexpr size_t kKindCount = 4;

  // |context| is the context of the machine this tracker is for.
  explicit MachineDataClaimTracker(TraceProcessorContext* context);
  ~MachineDataClaimTracker();

  MachineDataClaimTracker(const MachineDataClaimTracker&) = delete;
  MachineDataClaimTracker& operator=(const MachineDataClaimTracker&) = delete;

  // The kind of machine-wide data carried by the TracePacket field
  // |field_id|, for packets whose data is timestamped by the packet
  // timestamp. ftrace and ETW event bundles are not included: they are
  // arbitrated per event, as a bundle spans a range of time.
  static std::optional<Kind> KindForTracePacketField(uint32_t field_id);

  static const char* KindToString(Kind kind);

  // Whether the first trace to provide |kind| owns it for all time rather
  // than for the time window it covers.
  static bool IsExclusive(Kind kind) { return kind == Kind::kCpuPerUid; }

  // Must be called at tokenization time for each piece of data of |kind| at
  // trace time |ts| read by the (machine, trace) context |context|, whose
  // machine must be this tracker's.
  //
  // Returns true if the data should be imported, false if it must be dropped
  // because another trace provides |kind| at |ts| (the drop is recorded
  // against |context|).
  PERFETTO_ALWAYS_INLINE bool ShouldImport(TraceProcessorContext* context,
                                           Kind kind,
                                           int64_t ts) {
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

  // Called at the end of the import, once all data has been parsed. Closes
  // state left open at the end of each window which is followed by a window
  // of another trace (see rule 4 above).
  void OnEventsFullyExtracted();

  // Tracks shared by all traces on this machine. Only accessed by
  // TrackTracker.
  using SharedTrackMap =
      base::FlatHashMap<uint64_t, TrackId, base::AlreadyHashed<uint64_t>>;
  SharedTrackMap& shared_tracks() { return shared_tracks_; }
  void OnTrackShared(TrackId track_id, Kind kind) {
    shared_track_kinds_.Insert(track_id, kind);
  }

  // A trace's time window for a kind. Exposed for testing.
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
    // The timestamps of the first and last data accepted from the trace.
    int64_t min;
    int64_t max;
    // The range within which data from the trace is accepted. Only the
    // window of the trace which claimed the kind most recently can grow
    // (bounded by the windows around it); all other windows are frozen to
    // [min, max].
    int64_t lo;
    int64_t hi;
  };

  // Aggregated information about the data of a kind dropped from a trace.
  struct Drops {
    TraceId trace_id;
    uint64_t count = 0;
  };

  struct Claims {
    std::vector<Window> windows;
    // Index in |windows| of the window which last accepted data.
    std::optional<size_t> last_accepted;
    std::vector<Drops> drops;
  };

  bool ShouldImportSlow(TraceProcessorContext* context, Kind kind, int64_t ts);
  void RecordDrop(TraceProcessorContext* context,
                  Kind kind,
                  int64_t ts,
                  std::optional<TraceId> conflicting_trace);

  // Returns the window containing |ts| if that window is followed by a window
  // of another trace, nullptr otherwise.
  const Window* FollowedWindowContaining(Kind kind, int64_t ts) const;
  // Returns the duration to close a row starting at |ts| with, if any, and
  // counts the closure against the trace owning the window.
  std::optional<int64_t> CloseAtWindowEnd(Kind kind, int64_t ts);
  void CloseOpenSchedSlices();
  void CloseOpenThreadStates();
  void CloseOpenSlicesOnSharedTracks();

  TraceProcessorContext* const context_;
  const StringId kind_key_id_;
  const StringId conflicting_trace_id_key_id_;
  std::array<StringId, kKindCount> kind_names_;
  std::array<Claims, kKindCount> claims_;

  SharedTrackMap shared_tracks_;
  base::FlatHashMap<TrackId, Kind> shared_track_kinds_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_MACHINE_DATA_CLAIM_TRACKER_H_
