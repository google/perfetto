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

#include <math.h>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/utils.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"

namespace perfetto {
namespace trace_processor {

EventTracker::EventTracker(TraceProcessorContext* context)
    : context_(context) {}

EventTracker::~EventTracker() = default;

base::Optional<CounterId> EventTracker::PushProcessCounterForThread(
    int64_t timestamp,
    double value,
    StringId name_id,
    UniqueTid utid) {
  auto opt_id = PushCounter(timestamp, value, kInvalidTrackId);
  if (opt_id) {
    PendingUpidResolutionCounter pending;
    pending.row = *context_->storage->counter_table().id().IndexOf(*opt_id);
    pending.utid = utid;
    pending.name_id = name_id;
    pending_upid_resolution_counter_.emplace_back(pending);
  }
  return opt_id;
}

base::Optional<CounterId> EventTracker::PushCounter(int64_t timestamp,
                                                    double value,
                                                    TrackId track_id) {
  if (timestamp < max_timestamp_) {
    PERFETTO_DLOG(
        "counter event (ts: %" PRId64 ") out of order by %.4f ms, skipping",
        timestamp, static_cast<double>(max_timestamp_ - timestamp) / 1e6);
    context_->storage->IncrementStats(stats::counter_events_out_of_order);
    return base::nullopt;
  }
  max_timestamp_ = timestamp;

  auto* counter_values = context_->storage->mutable_counter_table();
  return counter_values->Insert({timestamp, track_id, value}).id;
}

base::Optional<CounterId> EventTracker::PushCounter(
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
    base::Optional<UniquePid> upid = thread_table.upid()[utid];

    TrackId track_id = kInvalidTrackId;
    if (upid.has_value()) {
      track_id = context_->track_tracker->InternProcessCounterTrack(
          pending_counter.name_id, *upid);
    } else {
      // If we still don't know which process this thread belongs to, fall back
      // onto creating a thread counter track. It's too late to drop data
      // because the counter values have already been inserted.
      track_id = context_->track_tracker->InternThreadCounterTrack(
          pending_counter.name_id, utid);
    }
    context_->storage->mutable_counter_table()->mutable_track_id()->Set(
        pending_counter.row, track_id);
  }
  pending_upid_resolution_counter_.clear();
}

}  // namespace trace_processor
}  // namespace perfetto
