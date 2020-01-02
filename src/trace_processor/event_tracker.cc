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

#include "src/trace_processor/event_tracker.h"

#include <math.h>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/utils.h"
#include "src/trace_processor/args_tracker.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/stats.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/track_tracker.h"
#include "src/trace_processor/variadic.h"

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
    PERFETTO_DLOG("counter event (ts: %" PRId64
                  ") out of order by %.4f ms, skipping",
                  timestamp, (max_timestamp_ - timestamp) / 1e6);
    context_->storage->IncrementStats(stats::counter_events_out_of_order);
    return base::nullopt;
  }
  max_timestamp_ = timestamp;

  auto* counter_values = context_->storage->mutable_counter_table();
  return counter_values->Insert({timestamp, track_id.value, value});
}

uint32_t EventTracker::PushInstant(int64_t timestamp,
                                   StringId name_id,
                                   double value,
                                   int64_t ref,
                                   RefType ref_type,
                                   bool resolve_utid_to_upid) {
  auto* instants = context_->storage->mutable_instants();
  uint32_t idx;
  if (resolve_utid_to_upid) {
    idx = instants->AddInstantEvent(timestamp, name_id, value, 0,
                                    RefType::kRefUpid);
    PendingUpidResolutionInstant pending;
    pending.row = idx;
    pending.utid = static_cast<UniqueTid>(ref);
    pending_upid_resolution_instant_.emplace_back(pending);
  } else {
    idx = instants->AddInstantEvent(timestamp, name_id, value, ref, ref_type);
  }
  return idx;
}

void EventTracker::FlushPendingEvents() {
  for (const auto& pending_counter : pending_upid_resolution_counter_) {
    const auto& thread = context_->storage->GetThread(pending_counter.utid);
    // TODO(lalitm): having upid == 0 is probably not the correct approach here
    // but it's unclear what may be better.
    UniquePid upid = thread.upid.value_or(0);
    TrackId id = context_->track_tracker->InternProcessCounterTrack(
        pending_counter.name_id, upid);
    context_->storage->mutable_counter_table()->mutable_track_id()->Set(
        pending_counter.row, id.value);
  }

  for (const auto& pending_instant : pending_upid_resolution_instant_) {
    const auto& thread = context_->storage->GetThread(pending_instant.utid);
    // TODO(lalitm): having upid == 0 is probably not the correct approach here
    // but it's unclear what may be better.
    UniquePid upid = thread.upid.value_or(0);
    context_->storage->mutable_instants()->set_ref(pending_instant.row, upid);
  }

  pending_upid_resolution_counter_.clear();
  pending_upid_resolution_instant_.clear();
}

}  // namespace trace_processor
}  // namespace perfetto
