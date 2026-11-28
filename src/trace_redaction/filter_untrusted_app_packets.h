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

#ifndef SRC_TRACE_REDACTION_FILTER_UNTRUSTED_APP_PACKETS_H_
#define SRC_TRACE_REDACTION_FILTER_UNTRUSTED_APP_PACKETS_H_

#include <string>

#include "src/trace_redaction/trace_redaction_framework.h"

namespace perfetto::trace_redaction {

// Drops every packet written by an app uid (trusted_uid > kMaxTrustedUid) that
// is not the target package. Afterwards only system, target-app and uid-less
// packets remain. Packets that pass are left untouched.
//
// Requires `PackageValidation` in the pipeline to guarantee
// `Context::package_uid` is populated before `Transform()` runs.
//
// Example (target package "com.example.target", uid 10146):
//
//   Before:
//     packet {
//       trusted_uid: 10200          # another app
//       trusted_packet_sequence_id: 5
//       timestamp: 1000
//     }
//     packet {
//       trusted_uid: 10146          # target app
//       trusted_packet_sequence_id: 4
//       timestamp: 1000
//     }
//
//   After:
//     packet {
//       trusted_uid: 10146
//       trusted_packet_sequence_id: 4
//       timestamp: 1000
//     }
class FilterUntrustedAppPackets final : public TransformPrimitive {
 public:
  base::Status Transform(const Context& context,
                         std::string* packet) const override;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_FILTER_UNTRUSTED_APP_PACKETS_H_
