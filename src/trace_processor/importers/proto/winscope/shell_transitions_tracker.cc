/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/importers/proto/winscope/shell_transitions_tracker.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

ShellTransitionsTracker::ShellTransitionsTracker(TraceProcessorContext* context)
    : context_(context) {}

ShellTransitionsTracker::~ShellTransitionsTracker() = default;

ArgsTracker::BoundInserter ShellTransitionsTracker::AddArgsTo(
    int32_t transition_id) {
  auto* transition_info = GetOrInsertTransition(transition_id);

  return transition_info->args_tracker.AddArgsTo(transition_info->row_id);
}

void ShellTransitionsTracker::SetTimestamp(int32_t transition_id,
                                           int64_t timestamp_ns) {
  auto pos = transitions_infos_.find(transition_id);
  if (pos == transitions_infos_.end()) {
    context_->storage->IncrementStats(
        stats::winscope_shell_transitions_parse_errors);
    return;
  }

  auto* window_manager_shell_transitions_table =
      context_->storage->mutable_window_manager_shell_transitions_table();
  window_manager_shell_transitions_table->FindById(pos->second.row_id)
      .value()
      .set_ts(timestamp_ns);
}

void ShellTransitionsTracker::Flush() {
  // The destructor of ArgsTracker will flush the args to the tables.
  transitions_infos_.clear();
}

ShellTransitionsTracker::TransitionInfo*
ShellTransitionsTracker::GetOrInsertTransition(int32_t transition_id) {
  auto pos = transitions_infos_.find(transition_id);
  if (pos != transitions_infos_.end()) {
    return &pos->second;
  }

  auto* window_manager_shell_transitions_table =
      context_->storage->mutable_window_manager_shell_transitions_table();

  tables::WindowManagerShellTransitionsTable::Row row;
  row.transition_id = transition_id;
  auto row_id = window_manager_shell_transitions_table->Insert(row).id;

  transitions_infos_.insert(
      {transition_id, TransitionInfo{row_id, ArgsTracker(context_)}});

  pos = transitions_infos_.find(transition_id);
  return &pos->second;
}

}  // namespace trace_processor
}  // namespace perfetto
