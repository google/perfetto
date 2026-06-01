/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/importers/common/state_tracker.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

StateTracker::StateTracker(TraceProcessorContext* context)
    : context_(context) {}

StateTracker::~StateTracker() = default;

std::optional<tables::StateTable::Id> StateTracker::UpdateState(
    int64_t timestamp,
    TrackId track_id,
    StringId value_id,
    StringId category_id,
    SetArgsCallback args_callback) {
  auto* states = context_->storage->mutable_state_table();

  auto* active = active_states_.Find(track_id);
  if (active) {
    auto ref = active->row.ToRowReference(states);
    if (active->value == value_id) {
      // Augment current state with new arguments
      if (args_callback) {
        ArgsTracker args_tracker(context_);
        auto bound_inserter = args_tracker.AddArgsTo(ref.id());
        args_callback(&bound_inserter);
      }
      return ref.id();
    }

    // Different state: close active one
    ref.set_dur(timestamp - ref.ts());
    active_states_.Erase(track_id);
  }

  if (!value_id.is_null()) {
    tables::StateTable::Row row;
    row.ts = timestamp;
    row.dur = -1;  // pending duration
    row.track_id = track_id;
    row.category =
        category_id.is_null() ? std::nullopt : std::make_optional(category_id);
    row.value = value_id;

    auto id_and_row = states->Insert(row);
    auto ref = (*states)[id_and_row.row];

    if (args_callback) {
      ArgsTracker args_tracker(context_);
      auto bound_inserter = args_tracker.AddArgsTo(ref.id());
      args_callback(&bound_inserter);
    }

    active_states_.Insert(
        track_id, {tables::StateTable::RowNumber(id_and_row.row), value_id});

    return ref.id();
  }

  return std::nullopt;
}

}  // namespace perfetto::trace_processor
