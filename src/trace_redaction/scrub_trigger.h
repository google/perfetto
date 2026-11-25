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

#ifndef SRC_TRACE_REDACTION_SCRUB_TRIGGER_H_
#define SRC_TRACE_REDACTION_SCRUB_TRIGGER_H_

#include <string>

#include "src/trace_redaction/trace_redaction_framework.h"

namespace perfetto::trace_redaction {

// Removes Trigger.producer_name and Trigger.trusted_producer_uid unless the
// producer is the target package. They name the app whose OOM started the
// session, which may be another app.
//
// Requires `PackageValidation` in the pipeline to guarantee
// `Context::package_uid` is populated before `Transform()` runs.
//
// Example (target uid 10146):
//
//   Before:
//     packet {
//       trusted_uid: 9999
//       timestamp: 90
//       trigger {
//         trigger_name: "com.android.telemetry.art-outofmemory"
//         producer_name: "com.example.other"
//         trusted_producer_uid: 10200
//       }
//     }
//
//   After:
//     packet {
//       trusted_uid: 9999
//       timestamp: 90
//       trigger {
//         trigger_name: "com.android.telemetry.art-outofmemory"
//       }
//     }
class ScrubTrigger final : public TransformPrimitive {
 public:
  base::Status Transform(const Context& context,
                         std::string* packet) const override;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_SCRUB_TRIGGER_H_
