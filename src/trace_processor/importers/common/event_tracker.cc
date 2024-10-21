/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/trace_processor/importers/common/event_tracker.h"

#include <cinttypes>
#include <cstdint>
#include <optional>

#include "perfetto/base/logging.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

EventTracker::EventTracker(TraceProcessorContext* context)
    : context_(context) {}

EventTracker::~EventTracker() = default;

std::optional<CounterId> EventTracker::PushProcessCounterForThread(
    int64_t timestamp,
    double value,
    StringId name_id,
    UniqueTid utid) {
  const auto& counter = context_->storage->counter_table();
  auto opt_id = PushCounter(timestamp, value, kInvalidTrackId);
  if (opt_id) {
    PendingUpidResolutionCounter pending;
    pending.row = counter.FindById(*opt_id)->ToRowNumber().row_number();
    pending.utid = utid;
    pending.name_id = name_id;
    pending_upid_resolution_counter_.emplace_back(pending);
  }
  return opt_id;
}

std::optional<CounterId> EventTracker::PushCounter(int64_t timestamp,
                                                   double value,
                                                   TrackId track_id) {
  if (timestamp < max_timestamp_) {
    PERFETTO_DLOG(
        "counter event (ts: %" PRId64 ") out of order by %.4f ms, skipping",
        timestamp, static_cast<double>(max_timestamp_ - timestamp) / 1e6);
    context_->storage->IncrementStats(stats::counter_events_out_of_order);
    return std::nullopt;
  }
  max_timestamp_ = timestamp;

  auto* counter_values = context_->storage->mutable_counter_table();
  return counter_values->Insert({timestamp, track_id, value, {}}).id;
}

std::optional<CounterId> EventTracker::PushCounter(
    int64_t timestamp,
    double value,
    TrackId track_id,
    SetArgsCallback args_callback) {
  auto maybe_counter_id = PushCounter(timestamp, value, track_id);
  if (maybe_counter_id) {
    auto inserter = context_->args_tracker->AddArgsTo(*maybe_counter_id);
    args_callback(&inserter);
  }
  return maybe_counter_id;
}

void EventTracker::FlushPendingEvents() {
  const auto& thread_table = context_->storage->thread_table();
  for (const auto& pending_counter : pending_upid_resolution_counter_) {
    UniqueTid utid = pending_counter.utid;
    std::optional<UniquePid> upid = thread_table[utid].upid();

    TrackId track_id = kInvalidTrackId;
    if (upid.has_value()) {
      track_id = context_->track_tracker->LegacyInternProcessCounterTrack(
          pending_counter.name_id, *upid);
    } else {
      // If we still don't know which process this thread belongs to, fall back
      // onto creating a thread counter track. It's too late to drop data
      // because the counter values have already been inserted.
      track_id = context_->track_tracker->LegacyInternThreadCounterTrack(
          pending_counter.name_id, utid);
    }
    auto& counter = *context_->storage->mutable_counter_table();
    counter[pending_counter.row].set_track_id(track_id);
  }
  pending_upid_resolution_counter_.clear();
}

}  // namespace perfetto::trace_processor
