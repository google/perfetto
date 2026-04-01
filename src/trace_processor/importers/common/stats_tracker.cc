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

#include "src/trace_processor/importers/common/stats_tracker.h"

#include <cstddef>
#include <cstdint>
#include <optional>

#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

StatsTracker::StatsTracker(TraceProcessorContext* context)
    : context_(context) {}

std::optional<MachineId> StatsTracker::machine_id() const {
  if (!context_->machine_tracker) {
    return std::nullopt;
  }
  return context_->machine_id();
}

std::optional<TraceId> StatsTracker::trace_id() const {
  if (!context_->trace_state) {
    return std::nullopt;
  }
  return context_->trace_id();
}

void StatsTracker::SetStats(size_t key, int64_t value) {
  context_->global_stats_tracker->SetStats(machine_id(), trace_id(), key,
                                           value);
}

void StatsTracker::IncrementStats(size_t key, int64_t increment) {
  context_->global_stats_tracker->IncrementStats(machine_id(), trace_id(), key,
                                                 increment);
}

void StatsTracker::SetIndexedStats(size_t key, int index, int64_t value) {
  context_->global_stats_tracker->SetIndexedStats(machine_id(), trace_id(), key,
                                                  index, value);
}

void StatsTracker::IncrementIndexedStats(size_t key,
                                         int index,
                                         int64_t increment) {
  context_->global_stats_tracker->IncrementIndexedStats(
      machine_id(), trace_id(), key, index, increment);
}

int64_t StatsTracker::GetStats(size_t key) {
  return context_->global_stats_tracker->GetStats(machine_id(), trace_id(),
                                                  key);
}

std::optional<int64_t> StatsTracker::GetIndexedStats(size_t key, int index) {
  return context_->global_stats_tracker->GetIndexedStats(
      machine_id(), trace_id(), key, index);
}

}  // namespace perfetto::trace_processor
