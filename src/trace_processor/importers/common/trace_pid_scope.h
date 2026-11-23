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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_TRACE_PID_SCOPE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_TRACE_PID_SCOPE_H_

#include <optional>

#include "src/trace_processor/importers/common/process_tracker.h"

namespace perfetto::trace_processor {

class TraceProcessorContext;

// The pid scope of a trace whose pids and tids aren't known to be kernel ids,
// such as JSON or Gecko. On its own, the trace gets a private scope. Merged
// with other traces, or with kernel data it embeds, its ids are taken to be
// kernel ids and it uses the machine scope.
//
// Decided on first use, which must be at parse time: every file in the load
// has been seen by then.
class TracePidScope {
 public:
  explicit TracePidScope(TraceProcessorContext*);

  void set_has_kernel_data() { has_kernel_data_ = true; }

  ProcessTracker::PidScope Get();

 private:
  TraceProcessorContext* const context_;
  bool has_kernel_data_ = false;
  std::optional<ProcessTracker::PidScope> scope_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_TRACE_PID_SCOPE_H_
