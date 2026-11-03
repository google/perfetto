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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_ANDROID_FRAMEWORK_TRACK_EVENT_UNKNOWN_PROCESS_LOGGER_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_ANDROID_FRAMEWORK_TRACK_EVENT_UNKNOWN_PROCESS_LOGGER_H_

#include <cstdint>
#include <optional>

#include "perfetto/base/flat_set.h"
#include "perfetto/protozero/field.h"

namespace perfetto::trace_processor {
class TraceProcessorContext;
}  // namespace perfetto::trace_processor

namespace perfetto::trace_processor::android_framework_track_event {

// Records an android_track_event_process_unknown notice, at most once per pid,
// when an Android framework event or state dump references a process that was
// neither in DUMP_REASON_START nor created by a process_start_event.
class UnknownProcessLogger {
 public:
  explicit UnknownProcessLogger(TraceProcessorContext* context);

  // |source| identifies the event or dump which referenced |pid|.
  // |process_name| and |start_seq_id| are recorded when available.
  void Record(int64_t ts,
              const char* source,
              uint32_t pid,
              protozero::ConstChars process_name = {},
              std::optional<int64_t> start_seq_id = std::nullopt);

 private:
  TraceProcessorContext* const context_;
  base::FlatSet<uint32_t> warned_pids_;
};

}  // namespace perfetto::trace_processor::android_framework_track_event

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_ANDROID_FRAMEWORK_TRACK_EVENT_UNKNOWN_PROCESS_LOGGER_H_
