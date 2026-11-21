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

#include "src/trace_processor/importers/common/trace_pid_scope.h"

#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/trace_file_tracker.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

TracePidScope::TracePidScope(TraceProcessorContext* context)
    : context_(context) {}

ProcessTracker::PidScope TracePidScope::Get() {
  if (!scope_) {
    bool merged = has_kernel_data_ ||
                  context_->trace_file_tracker->non_container_file_count() > 1;
    scope_ = merged ? ProcessTracker::kMachinePidScope
                    : context_->process_tracker->NewPidScope();
  }
  return *scope_;
}

}  // namespace perfetto::trace_processor
