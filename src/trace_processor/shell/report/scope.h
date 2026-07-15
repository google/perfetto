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

#ifndef SRC_TRACE_PROCESSOR_SHELL_REPORT_SCOPE_H_
#define SRC_TRACE_PROCESSOR_SHELL_REPORT_SCOPE_H_

#include <cstdint>
#include <string>

namespace perfetto::trace_processor::shell {

// The effective scope and aggregation controls for a report view, compiled
// from the report subcommand's flags. Extended as new scope flags land.
struct Scope {
  // Maximum number of aggregated rows to emit (--top).
  int64_t top = 10;
  // If non-empty, a GLOB restricting rows by name (--name).
  std::string name_glob;
};

}  // namespace perfetto::trace_processor::shell

#endif  // SRC_TRACE_PROCESSOR_SHELL_REPORT_SCOPE_H_
