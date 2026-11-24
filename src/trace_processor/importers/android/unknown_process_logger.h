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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_ANDROID_UNKNOWN_PROCESS_LOGGER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_ANDROID_UNKNOWN_PROCESS_LOGGER_H_

#include <cstdint>
#include <optional>

#include "perfetto/base/flat_set.h"
#include "perfetto/protozero/field.h"

namespace perfetto::trace_processor {

class TraceProcessorContext;

// Records an android_track_event_process_unknown notice when an Android
// framework event or state dump references a pid with no live process in
// ProcessTracker (never seen, or already ended), so the event is ignored.
//
// The notice is recorded at most once per pid: the first event for a pid wins,
// so a later event carrying |process_name| or |start_seq_id| for the same pid
// does not add them.
class UnknownProcessLogger {
 public:
  explicit UnknownProcessLogger(TraceProcessorContext* context);
  UnknownProcessLogger(const UnknownProcessLogger&) = delete;
  UnknownProcessLogger& operator=(const UnknownProcessLogger&) = delete;

  // |source| identifies the event or dump which referenced |pid|.
  // |process_name| and |start_seq_id| are recorded when available.
  void Record(int64_t ts,
              const char* source,
              int64_t pid,
              protozero::ConstChars process_name = {},
              std::optional<int64_t> start_seq_id = std::nullopt);

 private:
  TraceProcessorContext* const context_;
  base::FlatSet<int64_t> warned_pids_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_ANDROID_UNKNOWN_PROCESS_LOGGER_H_
