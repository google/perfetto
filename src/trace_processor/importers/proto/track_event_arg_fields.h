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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_ARG_FIELDS_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_ARG_FIELDS_H_

#include <string>

#include "perfetto/base/status.h"
#include "perfetto/protozero/field.h"
#include "src/trace_processor/importers/proto/track_event_extension_parser.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto::trace_processor {

class ActiveChromeProcessesTracker;
class TraceProcessorContext;

// Paths on Windows use backslash rather than slash as a separator.
// Normalise the paths by replacing backslashes with slashes to make it
// easier to write cross-platform scripts.
inline std::string NormalizePathSeparators(const protozero::ConstChars& path) {
  std::string result(path.data, path.size);
  for (char& c : result) {
    if (c == '\\')
      c = '/';
  }
  return result;
}

// The TrackEvent fields the core importer handles beyond a plain reflection
// into args: interned source locations, log messages, screenshots, histogram
// names, the active process list and the gpu correlation extension.
// Registered by the track event parser alongside the plugins' extension
// parsers.
class TrackEventArgFieldParser : public TrackEventExtensionParser {
 public:
  TrackEventArgFieldParser(TrackEventExtensionParserContext*,
                           TraceProcessorContext*,
                           ActiveChromeProcessesTracker*);
  ~TrackEventArgFieldParser() override;

  Result OnTrackEventField(const TrackEventExtensionField& field,
                           const TrackEventFieldContext& event) override;

 private:
  base::Status AddSourceLocationArgs(uint64_t iid,
                                     const TrackEventFieldContext& event);
  base::Status ParseTaskExecution(protozero::ConstBytes,
                                  const TrackEventFieldContext& event);
  base::Status ParseLogMessage(protozero::ConstBytes,
                               const TrackEventFieldContext& event);
  base::Status ParseHistogramName(protozero::ConstBytes,
                                  const TrackEventFieldContext& event);
  void ParseScreenshot(protozero::ConstBytes,
                       const TrackEventFieldContext& event);
  void ParseActiveProcesses(protozero::ConstBytes,
                            const TrackEventFieldContext& event);
  void ParseGpuCorrelation(protozero::ConstBytes,
                           const TrackEventFieldContext& event);

  TraceProcessorContext* const trace_context_;
  ActiveChromeProcessesTracker* const active_processes_tracker_;

  const StringId task_file_name_args_key_id_;
  const StringId task_function_name_args_key_id_;
  const StringId task_line_number_args_key_id_;
  const StringId log_message_body_key_id_;
  const StringId log_message_source_location_function_name_key_id_;
  const StringId log_message_source_location_file_name_key_id_;
  const StringId log_message_source_location_line_number_key_id_;
  const StringId log_message_priority_id_;
  const StringId source_location_function_name_key_id_;
  const StringId source_location_file_name_key_id_;
  const StringId source_location_line_number_key_id_;
  const StringId histogram_name_key_id_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_ARG_FIELDS_H_
