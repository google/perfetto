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

#include "shell_transitions_tracker.h"
#include "perfetto/ext/base/crash_keys.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/storage/metadata.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/winscope_proto_mapping.h"

namespace perfetto {
namespace trace_processor {
ShellTransitionsTracker::ShellTransitionsTracker(TraceProcessorContext* context)
    : context_(context) {}

ShellTransitionsTracker::~ShellTransitionsTracker() = default;

tables::WindowManagerShellTransitionsTable::Id
ShellTransitionsTracker::InternTransition(int32_t transition_id) {
  auto pos = transition_id_to_row_mapping_.find(transition_id);
  if (pos != transition_id_to_row_mapping_.end()) {
    return pos->second;
  }

  auto* window_manager_shell_transitions_table =
      context_->storage->mutable_window_manager_shell_transitions_table();

  tables::WindowManagerShellTransitionsTable::Row row;
  row.transition_id = transition_id;
  auto row_id = window_manager_shell_transitions_table->Insert(row).id;

  transition_id_to_row_mapping_.insert({transition_id, row_id});

  return row_id;
}
}  // namespace trace_processor
}  // namespace perfetto
