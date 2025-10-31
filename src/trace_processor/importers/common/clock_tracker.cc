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

#include <cstdint>
#include <ctime>
#include <optional>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/import_logs_tracker.h"
#include "src/trace_processor/importers/common/metadata_tracker.h"
#include "src/trace_processor/storage/metadata.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"
#include "src/trace_processor/util/clock_synchronizer.h"

namespace perfetto::trace_processor {

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
  context_->storage->IncrementStats(stats::clock_sync_cache_miss);
  return base::OkStatus();
}

base::Status ClockSynchronizerListenerImpl::OnInvalidClockSnapshot() {
  context_->storage->IncrementStats(stats::invalid_clock_snapshots);
  return base::OkStatus();
}

base::Status ClockSynchronizerListenerImpl::OnTraceTimeClockIdChanged(
    ClockSynchronizerBase::ClockId clock_id) {
  context_->metadata_tracker->SetMetadata(metadata::trace_time_clock_id,
                                          Variadic::Integer(clock_id));
  return base::OkStatus();
}

base::Status ClockSynchronizerListenerImpl::OnSetTraceTimeClock(
    ClockSynchronizerBase::ClockId clock_id) {
  context_->metadata_tracker->SetMetadata(metadata::trace_time_clock_id,
                                          Variadic::Integer(clock_id));
  return base::OkStatus();
}

void ClockSynchronizerListenerImpl::RecordConversionError(
    ClockSynchronizerBase::ErrorType error_type,
    ClockSynchronizerBase::ClockId source_clock_id,
    ClockSynchronizerBase::ClockId target_clock_id,
    int64_t source_timestamp,
    std::optional<size_t> byte_offset) {
  size_t stat_key;
  switch (error_type) {
    case ClockSynchronizerBase::ErrorType::kUnknownSourceClock:
      stat_key = stats::clock_sync_failure_unknown_source_clock;
      break;
    case ClockSynchronizerBase::ErrorType::kUnknownTargetClock:
      stat_key = stats::clock_sync_failure_unknown_target_clock;
      break;
    case ClockSynchronizerBase::ErrorType::kNoPath:
      stat_key = stats::clock_sync_failure_no_path;
      break;
    case ClockSynchronizerBase::ErrorType::kOk:
      PERFETTO_FATAL("RecordConversionError called with kOk");
      return;
  }
  auto args = [&](ArgsTracker::BoundInserter& inserter) {
    if (ClockTracker::IsSequenceClock(static_cast<uint32_t>(source_clock_id))) {
      auto [seq_id, seq_clock_id] =
          ClockTracker::ExtractSequenceClockId(source_clock_id);
      inserter.AddArg(source_sequence_id_key_,
                      Variadic::UnsignedInteger(seq_id));
      inserter.AddArg(source_clock_id_key_, Variadic::Integer(seq_clock_id));
    } else {
      inserter.AddArg(source_clock_id_key_, Variadic::Integer(source_clock_id));
    }
    inserter.AddArg(source_timestamp_key_, Variadic::Integer(source_timestamp));
    if (ClockTracker::IsSequenceClock(static_cast<uint32_t>(target_clock_id))) {
      auto [seq_id, seq_clock_id] =
          ClockTracker::ExtractSequenceClockId(target_clock_id);
      inserter.AddArg(target_sequence_id_key_,
                      Variadic::UnsignedInteger(seq_id));
      inserter.AddArg(target_clock_id_key_, Variadic::Integer(seq_clock_id));
    } else {
      inserter.AddArg(target_clock_id_key_, Variadic::Integer(target_clock_id));
    }
  };
  if (byte_offset) {
    context_->import_logs_tracker->RecordTokenizationError(stat_key,
                                                           *byte_offset, args);
  } else {
    context_->import_logs_tracker->RecordAnalysisError(stat_key, args);
  }
}

// Returns true if this is a local host, false otherwise.
bool ClockSynchronizerListenerImpl::IsLocalHost() {
  return !context_->machine_id();
}

}  // namespace perfetto::trace_processor
