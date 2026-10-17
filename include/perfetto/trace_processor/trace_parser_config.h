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

#ifndef INCLUDE_PERFETTO_TRACE_PROCESSOR_TRACE_PARSER_CONFIG_H_
#define INCLUDE_PERFETTO_TRACE_PROCESSOR_TRACE_PARSER_CONFIG_H_

#include <string>
#include <vector>
#include "perfetto/trace_processor/basic_types.h"

namespace perfetto::trace_processor {
// Options affecting which trace data is parsed and how it is ordered.
// Working-column retention and frontier advancement are implementation details.
struct PERFETTO_EXPORT_COMPONENT TraceParserConfig {
  SortingMode sorting_mode = SortingMode::kDefaultHeuristics;
  DropFtraceDataBefore drop_ftrace_data_before =
      DropFtraceDataBefore::kTracingStarted;
  SoftDropFtraceDataBefore soft_drop_ftrace_data_before =
      SoftDropFtraceDataBefore::kAllPerCpuBuffersValid;
  DropTrackEventDataBefore drop_track_event_data_before =
      DropTrackEventDataBefore::kNoDrop;
  // Serialized FileDescriptorSets for custom protobuf payloads.
  std::vector<std::string> extra_parsing_descriptors;
};
}  // namespace perfetto::trace_processor
#endif  // INCLUDE_PERFETTO_TRACE_PROCESSOR_TRACE_PARSER_CONFIG_H_
