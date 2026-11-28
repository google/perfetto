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

#ifndef SRC_TRACE_REDACTION_PRUNE_HEAP_DUMPS_H_
#define SRC_TRACE_REDACTION_PRUNE_HEAP_DUMPS_H_

#include <string>

#include "src/trace_redaction/trace_redaction_framework.h"

namespace perfetto::trace_redaction {

// For packets with a trusted_uid: drops any packet carrying heap_graph or
// art_process_metadata unless it was written by the target uid as part of a
// complete dump, and drops any packet carrying deobfuscation_mapping unless it
// was written by the target uid (uid-less mappings are handled by
// PruneMissingUidPackets). Packets without heap dump fields are left untouched.
//
// Requires `PackageValidation` in the pipeline to guarantee
// `Context::package_uid` is populated before `Transform()` runs.
//
// Example (target uid 10146; seq 4 is complete, seq 7 is truncated):
//
//   Before:
//     packet {                      # non-target system process dump
//       trusted_uid: 1001
//       trusted_packet_sequence_id: 6
//       heap_graph { pid: 300 continued: false ... }
//     }
//     packet {                      # incomplete target dump (continued: true)
//       trusted_uid: 10146
//       trusted_packet_sequence_id: 7
//       heap_graph { pid: 100 continued: true ... }
//     }
//     packet {                      # complete target dump
//       trusted_uid: 10146
//       trusted_packet_sequence_id: 4
//       heap_graph { pid: 100 continued: false ... }
//     }
//
//   After:
//     packet {
//       trusted_uid: 10146
//       trusted_packet_sequence_id: 4
//       heap_graph { pid: 100 continued: false ... }
//     }
class PruneHeapDumps final : public TransformPrimitive {
 public:
  base::Status Transform(const Context& context,
                         std::string* packet) const override;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_PRUNE_HEAP_DUMPS_H_
