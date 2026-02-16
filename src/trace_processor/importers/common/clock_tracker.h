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
#include <variant>
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
      std::optional<size_t> byte_offset = std::nullopt) {
    if (PERFETTO_UNLIKELY(num_conversions_ == 0)) {
      HandlePendingFinalization();
    }
    auto* state = context_->trace_time_state.get();
    ++num_conversions_;
    auto ts = active_sync_->Convert(clock_id, timestamp, state->clock_id,
                                    byte_offset);
    return ts ? std::optional(ToHostTraceTime(*ts)) : ts;
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

  base::StatusOr<uint32_t> AddSnapshot(
      const std::vector<ClockTimestamp>& clock_timestamps);

  // Definitively sets the trace time clock. Supersedes any pending
  // AssumeClockIsTraceTime. Returns an error if the clock has already been
  // finalized with a different clock.
  base::Status SetDefiniteTraceTimeClock(ClockId clock_id);

  // TODO: add comment
  void SetAddIdentityPathToTraceTimeFallback(ClockId clock_id);

  // TODO: add comment
  void SetSwitchTraceTimeToClock(ClockId clock_id);

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
    int64_t clock_offset = state->remote_clock_offsets[state->clock_id];
    return timestamp - clock_offset;
  }

  PERFETTO_NO_INLINE void HandlePendingFinalization();

  PERFETTO_NO_INLINE void MaybeSetTraceClock(ClockId clock_id);

  TraceProcessorContext* context_;

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

  // TODO(lalitm): add detailed commentary here.
  struct NoSpecialHandling {};
  struct SwitchTraceTimeToClock {
    ClockId clock_id;
  };
  struct AddIdentityPathToTraceTime {
    ClockId clock_id;
  };
  using FallbackClockVariant = std::variant<NoSpecialHandling,
                                            SwitchTraceTimeToClock,
                                            AddIdentityPathToTraceTime>;
  FallbackClockVariant fallback_clock_;
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
