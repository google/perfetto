/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/importers/common/clock_tracker.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/import_logs_tracker.h"
#include "src/trace_processor/importers/common/metadata_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/storage/metadata.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"
#include "src/trace_processor/util/clock_synchronizer.h"

#include "protos/perfetto/common/builtin_clock.pbzero.h"

namespace perfetto::trace_processor {
namespace {

std::optional<StringId> GetBuiltinClockNameOrNull(TraceStorage* storage,
                                                  int64_t clock_id) {
  switch (clock_id) {
    case protos::pbzero::BUILTIN_CLOCK_REALTIME:
      return storage->InternString("REALTIME");
    case protos::pbzero::BUILTIN_CLOCK_REALTIME_COARSE:
      return storage->InternString("REALTIME_COARSE");
    case protos::pbzero::BUILTIN_CLOCK_MONOTONIC:
      return storage->InternString("MONOTONIC");
    case protos::pbzero::BUILTIN_CLOCK_MONOTONIC_COARSE:
      return storage->InternString("MONOTONIC_COARSE");
    case protos::pbzero::BUILTIN_CLOCK_MONOTONIC_RAW:
      return storage->InternString("MONOTONIC_RAW");
    case protos::pbzero::BUILTIN_CLOCK_BOOTTIME:
      return storage->InternString("BOOTTIME");
    default:
      return std::nullopt;
  }
}

}  // namespace

// --- ClockTracker: public slow-path methods ---

ClockTracker::ClockTracker(
    TraceProcessorContext* context,
    std::unique_ptr<ClockSynchronizerListenerImpl> listener,
    ClockSynchronizer* primary_sync,
    bool is_primary)
    : context_(context),
      sync_(context->trace_time_state.get(), std::move(listener)),
      active_sync_(primary_sync),
      is_primary_(is_primary) {
  PERFETTO_CHECK(primary_sync);
}

base::StatusOr<uint32_t> ClockTracker::AddSnapshot(
    const std::vector<ClockTimestamp>& clock_timestamps) {
  if (PERFETTO_UNLIKELY(!is_primary_)) {
    // Non-primary trace: if we were using the primary's pool and already
    // converted timestamps, those conversions may have used different clock
    // data than our own.
    if (PERFETTO_UNLIKELY(active_sync_ != &sync_ && num_conversions_ > 0)) {
      context_->import_logs_tracker->RecordAnalysisError(
          stats::clock_sync_mixed_clock_sources,
          [&](ArgsTracker::BoundInserter& inserter) {
            StringId key = context_->storage->InternString(
                "num_conversions_using_primary");
            inserter.AddArg(key, Variadic::UnsignedInteger(num_conversions_));
          });
    }
    // Switch to our own sync and add the snapshot there.
    active_sync_ = &sync_;
  }
  return AddSnapshotInternal(clock_timestamps);
}

base::StatusOr<uint32_t> ClockTracker::AddSnapshotInternal(
    const std::vector<ClockTimestamp>& clock_timestamps) {
  ASSIGN_OR_RETURN(uint32_t snapshot_id,
                   active_sync_->AddSnapshot(clock_timestamps));
  AddSnapshotToTable(snapshot_id, clock_timestamps);
  return snapshot_id;
}

void ClockTracker::AddSnapshotToTable(
    uint32_t snapshot_id,
    const std::vector<ClockTimestamp>& clock_timestamps) {
  // Computed lazily the first time a clock fails to convert: most snapshots
  // resolve every clock directly and never need the snapshot fallback.
  std::optional<int64_t> trace_time_from_snapshot;
  bool trace_time_from_snapshot_computed = false;

  std::optional<int64_t> trace_ts_for_check;
  for (const auto& clock_timestamp : clock_timestamps) {
    // If the clock is incremental, we need to use 0 to map correctly to
    // |absolute_timestamp|.
    int64_t ts_to_convert =
        clock_timestamp.clock.is_incremental ? 0 : clock_timestamp.timestamp;
    // Even if we have trace time from snapshot, we still convert to optimise
    // future conversions. Don't pass byte_offset since we expect failures
    // here (e.g., non-monotonic clocks). Convert directly rather than via
    // ToTraceTime: recording is bookkeeping, so it must not flush the
    // deferred identity sync or count as an event conversion.
    auto* state = context_->trace_time_state.get();
    std::optional<int64_t> converted = active_sync_->Convert(
        clock_timestamp.clock.id, ts_to_convert, state->clock_id,
        /*byte_offset=*/std::nullopt, /*suppress_errors=*/true);
    std::optional<int64_t> opt_trace_ts =
        converted ? std::optional(ToHostTraceTime(*converted)) : std::nullopt;

    int64_t trace_ts_value;
    if (!opt_trace_ts) {
      // This can happen if |AddSnapshot| failed to resolve this clock, e.g.
      // if clock is not monotonic. Try to fetch trace time from snapshot.
      if (!trace_time_from_snapshot_computed) {
        trace_time_from_snapshot = ToTraceTimeFromSnapshot(clock_timestamps);
        trace_time_from_snapshot_computed = true;
      }
      if (!trace_time_from_snapshot) {
        continue;
      }
      trace_ts_value = *trace_time_from_snapshot;
    } else {
      trace_ts_value = *opt_trace_ts;
    }

    // Double check that all the clocks in this snapshot resolve to the same
    // trace timestamp value.
    PERFETTO_DCHECK(!trace_ts_for_check ||
                    trace_ts_value == trace_ts_for_check.value());
    trace_ts_for_check = trace_ts_value;

    tables::ClockSnapshotTable::Row row;
    row.ts = trace_ts_value;
    row.clock_id = static_cast<int64_t>(clock_timestamp.clock.id.clock_id);
    row.clock_value =
        clock_timestamp.timestamp * clock_timestamp.clock.unit_multiplier_ns;
    row.clock_name = GetBuiltinClockNameOrNull(
        context_->storage.get(), clock_timestamp.clock.id.clock_id);
    row.snapshot_id = snapshot_id;
    row.machine_id = context_->machine_id();

    context_->storage->mutable_clock_snapshot_table()->Insert(row);
  }
}

base::Status ClockTracker::SetGlobalClock(ClockId clock_id) {
  PERFETTO_DCHECK(!clock_id.IsSequenceClock());
  if (num_conversions_ > 0) {
    auto* state = context_->trace_time_state.get();
    return base::ErrStatus(
        "Not updating trace time clock from %s to %s"
        " because the old clock was already used for timestamp "
        "conversion - ClockSnapshot too late in trace?",
        state->clock_id.ToString().c_str(), clock_id.ToString().c_str());
  }
  auto* state = context_->trace_time_state.get();
  if (state->TrySetClock(clock_id, context_->trace_id().value)) {
    context_->metadata_tracker->SetMetadata(
        metadata::trace_time_clock_id, Variadic::Integer(clock_id.clock_id));
  }
  return base::OkStatus();
}

void ClockTracker::SetTraceDefaultClock(ClockId clock_id) {
  trace_default_clock_ = clock_id;
}

void ClockTracker::AddDeferredIdentitySync(ClockId clock_id) {
  deferred_identity_clock_ = clock_id;
}

void ClockTracker::FlushDeferredIdentitySync() {
  ClockId clock_id = *deferred_identity_clock_;
  deferred_identity_clock_.reset();
  auto* state = context_->trace_time_state.get();
  if (clock_id == state->clock_id) {
    return;
  }
  // Inject only if the clock cannot already reach trace time: a clock can
  // be in the graph yet disconnected from the trace time clock.
  if (active_sync_->Convert(clock_id, 0, state->clock_id, std::nullopt,
                            /*suppress_errors=*/true)) {
    return;
  }
  // The lazy edge becomes real here, so it is recorded like any other.
  AddSnapshotInternal({{clock_id, 0}, {state->clock_id, 0}});
}

std::optional<int64_t> ClockTracker::ToTraceTimeFromSnapshot(
    const std::vector<ClockTimestamp>& snapshot) {
  auto* state = context_->trace_time_state.get();
  auto it = std::find_if(snapshot.begin(), snapshot.end(),
                         [state](const ClockTimestamp& clock_timestamp) {
                           return clock_timestamp.clock.id == state->clock_id;
                         });
  if (it == snapshot.end()) {
    return std::nullopt;
  }
  return it->timestamp;
}

void ClockTracker::SetRemoteClockOffset(ClockId clock_id, int64_t offset) {
  context_->trace_time_state->remote_clock_offsets[clock_id] = offset;
}

std::optional<int64_t> ClockTracker::timezone_offset() const {
  return context_->trace_time_state->timezone_offset;
}

void ClockTracker::set_timezone_offset(int64_t offset) {
  context_->trace_time_state->timezone_offset = offset;
}

// --- ClockTracker: testing ---

void ClockTracker::set_cache_lookups_disabled_for_testing(bool v) {
  active_sync_->set_cache_lookups_disabled_for_testing(v);
}

const base::FlatHashMap<ClockId, int64_t>&
ClockTracker::remote_clock_offsets_for_testing() {
  return context_->trace_time_state->remote_clock_offsets;
}

uint32_t ClockTracker::cache_hits_for_testing() const {
  return active_sync_->cache_hits_for_testing();
}

// --- ClockSynchronizerListenerImpl ---

ClockSynchronizerListenerImpl::ClockSynchronizerListenerImpl(
    TraceProcessorContext* context)
    : context_(context),
      source_clock_id_key_(context->storage->InternString("source_clock_id")),
      target_clock_id_key_(context->storage->InternString("target_clock_id")),
      source_timestamp_key_(context->storage->InternString("source_timestamp")),
      source_sequence_id_key_(
          context->storage->InternString("source_sequence_id")),
      target_sequence_id_key_(
          context->storage->InternString("target_sequence_id")) {}

base::Status ClockSynchronizerListenerImpl::OnClockSyncCacheMiss() {
  context_->stats_tracker->IncrementStats(stats::clock_sync_cache_miss);
  return base::OkStatus();
}

base::Status ClockSynchronizerListenerImpl::OnInvalidClockSnapshot() {
  context_->stats_tracker->IncrementStats(stats::invalid_clock_snapshots);
  return base::OkStatus();
}

void ClockSynchronizerListenerImpl::RecordConversionError(
    ClockSyncErrorType error_type,
    ClockId source_clock_id,
    ClockId target_clock_id,
    int64_t source_timestamp,
    std::optional<size_t> byte_offset) {
  size_t stat_key;
  switch (error_type) {
    case ClockSyncErrorType::kUnknownSourceClock:
      stat_key = stats::clock_sync_failure_unknown_source_clock;
      break;
    case ClockSyncErrorType::kUnknownTargetClock:
      stat_key = stats::clock_sync_failure_unknown_target_clock;
      break;
    case ClockSyncErrorType::kNoPath:
      stat_key = stats::clock_sync_failure_no_path;
      break;
    case ClockSyncErrorType::kOk:
      PERFETTO_FATAL("RecordConversionError called with kOk");
      return;
  }
  auto args = [&](ArgsTracker::BoundInserter& inserter) {
    if (source_clock_id.seq_id != 0) {
      inserter.AddArg(source_sequence_id_key_,
                      Variadic::UnsignedInteger(source_clock_id.seq_id));
      inserter.AddArg(source_clock_id_key_,
                      Variadic::Integer(source_clock_id.clock_id));
    } else {
      inserter.AddArg(source_clock_id_key_,
                      Variadic::Integer(source_clock_id.clock_id));
    }
    inserter.AddArg(source_timestamp_key_, Variadic::Integer(source_timestamp));
    if (target_clock_id.seq_id != 0) {
      inserter.AddArg(target_sequence_id_key_,
                      Variadic::UnsignedInteger(target_clock_id.seq_id));
      inserter.AddArg(target_clock_id_key_,
                      Variadic::Integer(target_clock_id.clock_id));
    } else {
      inserter.AddArg(target_clock_id_key_,
                      Variadic::Integer(target_clock_id.clock_id));
    }
  };
  if (byte_offset) {
    context_->import_logs_tracker->RecordTokenizationError(stat_key,
                                                           *byte_offset, args);
  } else {
    context_->import_logs_tracker->RecordAnalysisError(stat_key, args);
  }
}

}  // namespace perfetto::trace_processor
