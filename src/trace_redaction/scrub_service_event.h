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

#ifndef SRC_TRACE_REDACTION_SCRUB_SERVICE_EVENT_H_
#define SRC_TRACE_REDACTION_SCRUB_SERVICE_EVENT_H_

#include <string>

#include "src/trace_redaction/trace_redaction_framework.h"

namespace perfetto::trace_redaction {

// Removes TracingServiceEvent.slow_starting_data_sources and
// last_flush_slow_data_sources. They list {producer_name, data_source_name}
// and an Android app producer's name is its process (package) name, so they
// can name other apps (e.g. another app whose heap dump was still being
// written at stop). The lifecycle events are kept.
//
// Example:
//
//   Before:
//     packet {
//       trusted_uid: 9999
//       timestamp: 600
//       service_event {
//         all_data_sources_flushed: true
//         last_flush_slow_data_sources {
//           data_source {
//             producer_name: "com.example.other"
//             data_source_name: "android.java_hprof.oom"
//           }
//         }
//       }
//     }
//
//   After:
//     packet {
//       trusted_uid: 9999
//       timestamp: 600
//       service_event {
//         all_data_sources_flushed: true
//       }
//     }
class ScrubServiceEvent final : public TransformPrimitive {
 public:
  base::Status Transform(const Context& context,
                         std::string* packet) const override;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_SCRUB_SERVICE_EVENT_H_
