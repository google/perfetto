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

#include "src/trace_processor/importers/common/machine_data_claim_tracker.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "perfetto/base/logging.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

MachineDataClaimTracker::MachineDataClaimTracker(TraceProcessorContext*)
    : owner_(1) {}

void MachineDataClaimTracker::RegisterMachineTrack(TrackId track_id,
                                                   std::string_view type) {
  if (track_id.value >= track_category_.size()) {
    track_category_.resize(track_id.value + 1, 0);
  }
  if (track_category_[track_id.value] != 0) {
    return;
  }

  uint8_t category = 0;
  for (size_t i = 0; i < counter_types_.size(); ++i) {
    if (counter_types_[i] == type) {
      category = static_cast<uint8_t>(i + 1);
      break;
    }
  }

  if (category == 0) {
    // Categories are stored as uint8_t; 0 means "not a machine track".
    PERFETTO_CHECK(counter_types_.size() < UINT8_MAX);
    counter_types_.push_back(type);
    owner_.push_back(std::nullopt);
    category = static_cast<uint8_t>(counter_types_.size());
  }

  track_category_[track_id.value] = category;
}

bool MachineDataClaimTracker::KeepCounter(TraceProcessorContext* caller,
                                          TrackId track) {
  auto* self = caller->machine_data_claim_tracker.get();
  if (!self) {
    return true;
  }
  if (track.value >= self->track_category_.size()) {
    return true;
  }
  uint8_t category = self->track_category_[track.value];
  if (category == 0) {
    return true;
  }

  tables::TraceFileTable::Id trace_id = caller->trace_id();
  PERFETTO_DCHECK(category < self->owner_.size());
  if (!self->owner_[category].has_value()) {
    self->owner_[category] = trace_id;
    return true;
  }
  if (*self->owner_[category] == trace_id) {
    return true;
  }
  caller->stats_tracker->IncrementStats(stats::machine_counter_claim_conflict);
  return false;
}

bool MachineDataClaimTracker::KeepSched(TraceProcessorContext* caller,
                                        SchedEventKind kind) {
  auto* self = caller->machine_data_claim_tracker.get();
  if (!self) {
    return true;
  }
  tables::TraceFileTable::Id trace_id = caller->trace_id();
  if (!self->owner_[0].has_value()) {
    if (kind == SchedEventKind::kSwitch) {
      self->owner_[0] = trace_id;
    }
    return true;
  }
  if (*self->owner_[0] == trace_id) {
    return true;
  }
  caller->stats_tracker->IncrementStats(stats::machine_sched_claim_conflict);
  return false;
}

}  // namespace perfetto::trace_processor
