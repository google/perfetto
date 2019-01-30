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

#include "src/trace_processor/clock_tracker.h"

#include <algorithm>

#include "perfetto/base/logging.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

ClockTracker::ClockTracker(TraceProcessorContext* ctx) : context_(ctx) {}
ClockTracker::~ClockTracker() = default;

void ClockTracker::SyncClocks(ClockDomain domain,
                              int64_t clock_time_ns,
                              int64_t trace_time_ns) {
  ClockSnapshotVector& snapshots = clocks_[domain];
  if (!snapshots.empty()) {
    // The trace clock (typically CLOCK_BOOTTIME) must be monotonic.
    if (trace_time_ns <= snapshots.back().trace_time_ns) {
      PERFETTO_ELOG("Trace time in clock snapshot is moving backwards");
      context_->storage->IncrementStats(stats::clock_snapshot_not_monotonic);
      return;
    }
    if (clock_time_ns <= snapshots.back().clock_time_ns) {
      if (domain == ClockDomain::kMonotonic) {
        PERFETTO_ELOG("CLOCK_MONOTONIC in clock snapshot is moving backwards");
        context_->storage->IncrementStats(stats::clock_snapshot_not_monotonic);
        return;
      }
      // This can happen in other clocks, for instance CLOCK_REALTIME if
      // adjusting the timezone or during daylight saving. In this case the most
      // reasonable thing we can do is obliterating all the past snapshots.
      while (!snapshots.empty() &&
             snapshots.back().clock_time_ns >= clock_time_ns) {
        snapshots.pop_back();
      }
    }
  }
  snapshots.emplace_back(ClockSnapshot{clock_time_ns, trace_time_ns});
}

base::Optional<int64_t> ClockTracker::ToTraceTime(ClockDomain domain,
                                                  int64_t clock_time_ns) {
  ClockSnapshotVector& snapshots = clocks_[domain];
  if (snapshots.empty()) {
    context_->storage->IncrementStats(stats::clock_sync_failure);
    return base::nullopt;
  }
  static auto comparator = [](int64_t lhs, const ClockSnapshot& rhs) {
    return lhs < rhs.clock_time_ns;
  };
  auto it = std::upper_bound(snapshots.begin(), snapshots.end(), clock_time_ns,
                             comparator);
  if (it != snapshots.begin())
    it--;
  return it->trace_time_ns + (clock_time_ns - it->clock_time_ns);
}

}  // namespace trace_processor
}  // namespace perfetto
