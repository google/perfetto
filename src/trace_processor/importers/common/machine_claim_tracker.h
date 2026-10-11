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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_MACHINE_CLAIM_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_MACHINE_CLAIM_TRACKER_H_

#include <cstdint>
#include <limits>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/hash.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

class SchedEventState;

// Per-machine state for machine-scoped tracks (tracks::MachineScoped()) and
// scheduling: the intern map that makes all traces of the machine share one
// track, and, when several traces are merged, the claims on those tracks and
// on scheduling.
//
// The first trace to write a track owns it until the trace's end (its last
// own timestamp, read from the sorter before parsing). Then the next trace
// takes over. Other traces' writes are hidden: dropped and counted in
// stats::machine_{track,sched}_events_hidden, and the first one per trace
// (and track) is in the import logs.
//
// Scheduling is claimed the same way, only on machines where two or more
// traces have ftrace: only a context switch claims it, until the trace's last
// ftrace event. Other scheduling events apply only from the owner; while no
// trace owns scheduling they are dropped, uncounted, like events before the
// soft-drop point. Open sched/thread_state rows are closed at each trace's
// last ftrace event when 2+ traces record scheduling.
//
// Created with the machine's first context: per-trace state (stats, import
// logs, trace id) always comes from the caller's context.
class MachineClaimTracker {
 public:
  // What a write by a trace to a machine-scoped track does. For scheduling,
  // only a context switch claims; other scheduling events are dropped,
  // uncounted, on kFirstWrite and kHandover.
  enum class Claim : uint8_t {
    // The trace owns the track (or the track is not machine-scoped): kept.
    kOwned,
    // Nobody owns the track yet: the trace claims it; kept.
    kFirstWrite,
    // The owner's claim ended before this write: the trace takes over; kept.
    kHandover,
    // Another trace owns the track at this time: dropped and counted.
    kHidden,
  };

  // The state, per trace and track, of a cumulative series continued across
  // owners (see SeriesValue()). Kept by the writing trace.
  struct SeriesInput {
    // The trace's previous sample.
    int64_t prev_ts = 0;
    double prev_value = 0;
    bool has_prev = false;
    // The trace's own value at |interp_ts|, the time of the owner's last
    // sample: interpolated between the trace's samples around that time, or
    // the trace's first sample after it if it has none before.
    int64_t interp_ts = 0;
    double interp_value = 0;
    bool has_interp = false;
  };

  explicit MachineClaimTracker(TraceProcessorContext* context);
  ~MachineClaimTracker();

  MachineClaimTracker(const MachineClaimTracker&) = delete;
  MachineClaimTracker& operator=(const MachineClaimTracker&) = delete;
  MachineClaimTracker(MachineClaimTracker&&) = delete;
  MachineClaimTracker& operator=(MachineClaimTracker&&) = delete;

  // The gate for writes to machine-scoped tracks (counters in
  // EventTracker::PushCounter(), GPU work periods in GpuWorkPeriodTracker):
  // returns whether the write by |writer|'s trace to |track| at |ts| is kept.
  // One predictable branch unless several traces of the machine are merged.
  PERFETTO_ALWAYS_INLINE static bool KeepWrite(TraceProcessorContext* writer,
                                               TrackId track,
                                               int64_t ts) {
    MachineClaimTracker* self = writer->machine_claim_tracker.get();
    if (PERFETTO_LIKELY(!self || !self->has_multiple_traces_)) {
      return true;
    }
    return self->Keep(writer, track, ts);
  }

  // The gate for context switches in FtraceSchedEventTracker: flags |writer|'s
  // trace as recording scheduling, claims scheduling if unowned (or after the
  // owner's ftrace end), and returns whether the switch is kept. One
  // predictable branch unless two or more traces of the machine have ftrace.
  PERFETTO_ALWAYS_INLINE static bool KeepSchedSwitch(
      TraceProcessorContext* writer,
      SchedEventState* sched_state,
      int64_t ts) {
    MachineClaimTracker* self = writer->machine_claim_tracker.get();
    if (PERFETTO_LIKELY(!self || !self->sched_shared_)) {
      return true;
    }
    return self->KeepSwitch(writer, sched_state, ts);
  }

  // The gate for other scheduling events (wakeups, blocked reasons, new-task
  // runnable states), which never claim: returns whether the event is kept,
  // i.e. whether |writer|'s trace owns scheduling at |ts|.
  PERFETTO_ALWAYS_INLINE static bool KeepSchedEvent(
      TraceProcessorContext* writer,
      int64_t ts) {
    MachineClaimTracker* self = writer->machine_claim_tracker.get();
    if (PERFETTO_LIKELY(!self || !self->sched_shared_)) {
      return true;
    }
    return self->KeepEvent(writer, ts);
  }

  // Reads the end of each trace on machines with several traces. Must run
  // before the first extraction, which empties the sorter queues. Read once
  // per machine; an unknown end means no handover.
  static void ReadTraceEnds(TraceProcessorContext* context);

  // When two or more traces on |context|'s machine recorded scheduling, closes
  // |context|'s open sched slices and thread states at its ftrace end and
  // counts cut rows in stats::machine_sched_open_rows_cut.
  static void CloseOpenSchedRows(TraceProcessorContext* context);

  // Interning of machine-scoped tracks, by blueprint and dimensions hash (see
  // TrackTracker::InternTrack()).
  TrackId* FindTrack(uint64_t hash) { return tracks_.Find(hash); }
  void AddTrack(uint64_t hash, TrackId track);

  // Called once for each trace with a context on this machine.
  void AddTrace(TraceId trace);
  bool has_multiple_traces() const { return has_multiple_traces_; }

  // Whether two or more traces of this machine have ftrace events: only then
  // are scheduling events gated.
  bool sched_shared() const { return sched_shared_; }

  // Sets the end of |trace|: the largest timestamp of its events.
  void SetTraceEnd(TraceId trace, int64_t end);

  // Sets the ftrace end of |trace|: the largest timestamp of its ftrace,
  // compact-sched or systrace events.
  void SetFtraceEnd(TraceId trace, int64_t end);

  // What a write by |trace| to |track| at |ts| does, without side effects.
  Claim Check(TraceId trace, TrackId track, int64_t ts) const;

  // What a scheduling event by |trace| at |ts| does, without side effects.
  Claim CheckSched(TraceId trace, int64_t ts) const;

  // Applies Check(): on kFirstWrite and kHandover, |writer|'s trace becomes
  // the owner; on kHidden, the write is counted for |writer|. Returns whether
  // the write is kept.
  bool Keep(TraceProcessorContext* writer, TrackId track, int64_t ts);

  // Apply CheckSched() to a context switch, which also flags |writer|'s trace
  // as recording scheduling, and to another scheduling event.
  bool KeepSwitch(TraceProcessorContext* writer,
                  SchedEventState* sched_state,
                  int64_t ts);
  bool KeepEvent(TraceProcessorContext* writer, int64_t ts);

  // For CPU-per-UID totals, which each trace sums from zero: returns the value
  // to write for |trace|'s sample so that the machine keeps one continuous
  // series. At a handover the new owner adds its increase since the old
  // owner's last sample, reading its own value at that time by interpolation
  // (or its first sample if it has none before). The result must still go
  // through the gate.
  double SeriesValue(TraceId trace,
                     TrackId track,
                     int64_t ts,
                     double value,
                     SeriesInput* input);

 private:
  // The series state of a track: the value last written on it and its time
  // (the owner's last sample), and the offset from the owner's values to the
  // series.
  struct SeriesTail {
    int64_t ts = 0;
    double value = 0;
    double offset = 0;
  };

  // |owners_| entries.
  static constexpr uint32_t kNotMachineTrack =
      std::numeric_limits<uint32_t>::max();
  static constexpr uint32_t kNoOwner = kNotMachineTrack - 1;

  // An owner whose end is unknown keeps its tracks.
  static constexpr int64_t kUnknownEnd = std::numeric_limits<int64_t>::max();

  Claim Decide(uint32_t owner,
               TraceId trace,
               int64_t ts,
               int64_t owner_end) const;
  int64_t TraceEnd(uint32_t trace) const;
  int64_t FtraceEnd(uint32_t trace) const;
  void NoteSchedSwitch(TraceId trace, SchedEventState* sched_state);
  void RecordHidden(TraceProcessorContext* writer,
                    TrackId track,
                    int64_t ts,
                    uint32_t owner);
  void RecordSchedHidden(TraceProcessorContext* writer,
                         int64_t ts,
                         uint32_t owner);

  struct SchedTraceState {
    SchedEventState* sched_state = nullptr;
    bool records_sched = false;
    bool logged_hidden = false;
  };

  const StringId track_id_key_;
  const StringId owner_trace_id_key_;

  // Machine-scoped tracks, by blueprint and dimensions hash.
  base::FlatHashMap<uint64_t, TrackId, base::AlreadyHashed<uint64_t>> tracks_;

  // Indexed by TrackId (track ids are global): kNotMachineTrack, kNoOwner, or
  // the owner's TraceId value.
  std::vector<uint32_t> owners_;

  // The traces with a context on this machine.
  std::vector<TraceId> traces_;
  bool has_multiple_traces_ = false;
  bool ends_read_ = false;

  // Indexed by TraceId value; kUnknownEnd if not read.
  std::vector<int64_t> trace_ends_;
  std::vector<int64_t> ftrace_ends_;

  // Scheduling claim state on this machine.
  bool sched_shared_ = false;
  uint32_t sched_owner_ = kNoOwner;
  uint32_t sched_trace_count_ = 0;
  std::vector<SchedTraceState> sched_traces_;

  // (trace << 32 | track) of the hidden writes recorded in the import logs.
  base::FlatHashMap<uint64_t, bool> logged_hidden_;

  // Indexed by TrackId value.
  base::FlatHashMap<uint32_t, SeriesTail> series_tails_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_MACHINE_CLAIM_TRACKER_H_
