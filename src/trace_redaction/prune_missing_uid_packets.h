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

#ifndef SRC_TRACE_REDACTION_PRUNE_MISSING_UID_PACKETS_H_
#define SRC_TRACE_REDACTION_PRUNE_MISSING_UID_PACKETS_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "src/trace_redaction/filtering.h"
#include "src/trace_redaction/trace_redaction_framework.h"

namespace perfetto::trace_redaction {

// Packets without a trusted_uid were not written through traced (e.g. a packet
// appended to the trace after collection). Drops every uid-less packet unless
// at least one allowlisted PacketFilter accepts it. With an empty allowlist,
// all uid-less packets are dropped.
//
// Example (empty allowlist):
//
//   Before:
//     packet {                      # no trusted_uid -> dropped
//       timestamp: 1000
//       process_tree { ... }
//     }
//     packet {                      # has trusted_uid -> kept
//       trusted_uid: 9999
//       timestamp: 1000
//       process_tree { ... }
//     }
//
//   After:
//     packet {
//       trusted_uid: 9999
//       timestamp: 1000
//       process_tree { ... }
//     }
class PruneMissingUidPackets final : public TransformPrimitive {
 public:
  base::Status Transform(const Context& context,
                         std::string* packet) const override;

  template <typename Filter>
  Filter* emplace_allow() {
    auto uptr = std::make_unique<Filter>();
    auto* ptr = uptr.get();
    allow_list_.push_back(std::move(uptr));
    return ptr;
  }

 private:
  std::vector<std::unique_ptr<PacketFilter>> allow_list_;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_PRUNE_MISSING_UID_PACKETS_H_
