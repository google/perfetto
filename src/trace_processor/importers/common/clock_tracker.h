/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_CLOCK_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_CLOCK_TRACKER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/clock_synchronizer.h"

namespace perfetto::trace_processor {

class ClockTrackerTest;

class ClockSynchronizerListenerImpl;

// ClockTracker wraps ClockSynchronizer (the pure conversion engine) and adds
// trace-time semantics: which clock domain is the "trace time", applying
// remote-machine offsets, and writing metadata.
class ClockTracker {
 public:
  // Re-export types for callers that use ClockTracker::ClockId etc.
  using ClockId = ::perfetto::trace_processor::ClockId;
  using ClockTimestamp = ::perfetto::trace_processor::ClockTimestamp;
  using Clock = ::perfetto::trace_processor::Clock;

  // |primary_sync| is the primary trace's ClockSynchronizer. For the primary
  // trace (first trace for a machine), snapshots are added directly to
  // |primary_sync| and it is used for all conversions. For non-primary traces,
  // |primary_sync| is used for conversions until the first AddSnapshot call,
  // at which point the tracker switches to its own internal sync.
  ClockTracker(TraceProcessorContext* context,
               std::unique_ptr<ClockSynchronizerListenerImpl> listener,
               ClockSynchronizer* primary_sync,
               bool is_primary);

  // --- Hot-path APIs (inlined) ---

  // Converts a timestamp to the trace time domain. On the first call, also
  // finalizes the trace time clock, preventing it from being changed later.
  PERFETTO_ALWAYS_INLINE std::optional<int64_t> ToTraceTime(
      ClockId clock_id,
      int64_t timestamp,
      std::optional<size_t> byte_offset = std::nullopt,
      bool suppress_errors = false) {
    if (PERFETTO_UNLIKELY(deferred_clock_sync_.has_value())) {
      FlushDeferredClockSync();
    }
    auto* state = context_->trace_time_state.get();
    ++num_conversions_;
    auto ts = active_sync_->Convert(clock_id, timestamp, state->clock_id,
                                    byte_offset, suppress_errors);
    return ts ? std::optional(ToHostTraceTime(*ts)) : ts;
  }

  // Converts |ts|, expressed in this trace file's default clock (set by
  // ForwardingTraceParser via SetTraceDefaultClock), to trace time. Tokenizers
  // for single-clock formats call this instead of naming a builtin clock, so a
  // perfetto_manifest override that swaps the default clock for the file's
  // private clock transparently redirects them.
  PERFETTO_ALWAYS_INLINE std::optional<int64_t> ConvertDefaultClockToTraceTime(
      int64_t ts,
      std::optional<size_t> byte_offset = std::nullopt,
      bool suppress_errors = false) {
    PERFETTO_DCHECK(trace_default_clock_.has_value());
    return ToTraceTime(*trace_default_clock_, ts, byte_offset, suppress_errors);
  }

  // Converts a timestamp between two arbitrary clock domains.
  PERFETTO_ALWAYS_INLINE std::optional<int64_t> Convert(
      ClockId src,
      int64_t ts,
      ClockId target,
      std::optional<size_t> byte_offset = {}) {
    ++num_conversions_;
    return active_sync_->Convert(src, ts, target, byte_offset);
  }

  // --- Slow-path public APIs ---

  // Adds a clock snapshot to the clock graph and records it in the
  // clock_snapshot table (one row per clock, converted to trace time, best
  // effort), which backs ClockConverter (to_realtime, abs_time_str, the UI
  // wall-clock axis). Every edge entering the graph is recorded this way;
  // deferred syncs record when their edge is actually injected.
  base::StatusOr<uint32_t> AddSnapshot(
      const std::vector<ClockTimestamp>& clock_timestamps);

  // --- Low-level clock primitives. Do not call without understanding the
  // --- consequences. Most callers should use the helpers below these.

  // Sets the global trace time clock (trace_time_state->clock_id).
  // All TP timestamps will be converted to this clock domain.
  // Returns error if called after conversions have already happened.
  base::Status SetGlobalClock(ClockId clock_id);

  // Sets the default clock for this trace file only (no global effect).
  // Used as fallback when no timestamp_clock_id is specified.
  void SetTraceDefaultClock(ClockId clock_id);

  // Registers a deferred clock edge: on the first ToTraceTime call, if |from|
  // (at |from_ts|) cannot already reach |to| through the clock graph, an edge
  // correlating |from|:|from_ts| with |to|:|to_ts| is injected into the shared
  // machine graph. |to| defaults to the trace time clock (resolved at flush);
  // |from_ts|/|to_ts| default to zero. With all defaults this is the plain
  // identity-to-trace-time edge every trace file registers for its source
  // clock; a perfetto_manifest override passes a non-zero offset and/or an
  // explicit target. All timestamps must be non-negative.
  void AddDeferredClockSync(ClockId from,
                            int64_t from_ts = 0,
                            std::optional<ClockId> to = std::nullopt,
                            int64_t to_ts = 0);

  // Returns the trace default clock, if one has been set.
  std::optional<ClockId> trace_default_clock() const {
    return trace_default_clock_;
  }

  std::optional<int64_t> ToTraceTimeFromSnapshot(
      const std::vector<ClockTimestamp>& snapshot);

  void SetRemoteClockOffset(ClockId clock_id, int64_t offset);
  std::optional<int64_t> timezone_offset() const;
  void set_timezone_offset(int64_t offset);

  // --- Testing ---
  void set_cache_lookups_disabled_for_testing(bool v);
  const base::FlatHashMap<ClockId, int64_t>& remote_clock_offsets_for_testing();
  uint32_t cache_hits_for_testing() const;

 private:
  friend class ClockTrackerTest;

  PERFETTO_ALWAYS_INLINE int64_t ToHostTraceTime(int64_t timestamp) {
    if (PERFETTO_LIKELY(context_->machine_id() ==
                        MachineId(kDefaultMachineId))) {
      return timestamp;
    }
    auto* state = context_->trace_time_state.get();
    // Find, not operator[]: a lookup must not insert a spurious zero offset.
    int64_t* clock_offset = state->remote_clock_offsets.Find(state->clock_id);
    return timestamp - (clock_offset ? *clock_offset : 0);
  }

  PERFETTO_NO_INLINE void FlushDeferredClockSync();

  // Adds an edge directly to |active_sync_| (bypassing the single-clock
  // rejection and the non-primary sync switch of the public AddSnapshot)
  // and records it in the clock_snapshot table. Every graph mutation goes
  // through here so the table is a faithful record of the graph.
  base::StatusOr<uint32_t> AddSnapshotInternal(
      const std::vector<ClockTimestamp>& clock_timestamps);

  // Records a snapshot already added to the graph (which assigned it
  // |snapshot_id|) into the clock_snapshot table: pure bookkeeping, one
  // best-effort row per clock; performs no deferred-sync flush and counts
  // no conversions.
  void AddSnapshotToTable(uint32_t snapshot_id,
                          const std::vector<ClockTimestamp>& clock_timestamps);

  // Returns the interned name for a builtin clock, or nullopt if |clock_id| is
  // not a builtin clock. The names are interned once in the constructor.
  std::optional<StringId> GetBuiltinClockNameOrNull(int64_t clock_id) const;

  TraceProcessorContext* context_;

  // Interned builtin clock names, populated in the constructor.
  StringId realtime_clock_name_;
  StringId realtime_coarse_clock_name_;
  StringId monotonic_clock_name_;
  StringId monotonic_coarse_clock_name_;
  StringId monotonic_raw_clock_name_;
  StringId boottime_clock_name_;

  // Private ClockSynchronizer used for non-primary traces. Primary traces use
  // the externally provided |primary_sync_| directly and don't use this member.
  ClockSynchronizer sync_;

  // Points to the ClockSynchronizer used for conversions. Starts at
  // |primary_sync_| and switches to |sync_| for non-primary traces
  // on the first AddSnapshot call.
  ClockSynchronizer* active_sync_;

  // Whether this is the primary trace for its machine. Non-primary
  // traces start using primary_sync_ and switch to sync_ on first AddSnapshot.
  bool is_primary_ = true;

  // Total number of conversions performed. When a non-primary trace switches
  // to its own sync, this value indicates how many conversions used the
  // primary trace's clocks.
  uint32_t num_conversions_ = 0;

  // The default clock for this trace file, set via SetTraceDefaultClock.
  // Used by proto_trace_reader as a fallback when no timestamp_clock_id
  // is specified.
  std::optional<ClockId> trace_default_clock_;

  // Edge registered via AddDeferredClockSync, flushed (and cleared) on the
  // first ToTraceTime call. |to| == nullopt means the trace time clock,
  // resolved at flush time.
  struct DeferredSync {
    ClockId from;
    int64_t from_ts;
    std::optional<ClockId> to;
    int64_t to_ts;
  };
  std::optional<DeferredSync> deferred_clock_sync_;
};

class ClockSynchronizerListenerImpl : public ClockSynchronizerListener {
 public:
  explicit ClockSynchronizerListenerImpl(TraceProcessorContext* context);

  base::Status OnClockSyncCacheMiss() override;

  base::Status OnInvalidClockSnapshot() override;

  void RecordConversionError(ClockSyncErrorType,
                             ClockId source_clock_id,
                             ClockId target_clock_id,
                             int64_t source_timestamp,
                             std::optional<size_t>) override;

 private:
  TraceProcessorContext* context_;
  StringId source_clock_id_key_;
  StringId target_clock_id_key_;
  StringId source_timestamp_key_;
  StringId source_sequence_id_key_;
  StringId target_sequence_id_key_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_CLOCK_TRACKER_H_
