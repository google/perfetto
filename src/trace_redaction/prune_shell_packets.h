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

#ifndef SRC_TRACE_REDACTION_PRUNE_SHELL_PACKETS_H_
#define SRC_TRACE_REDACTION_PRUNE_SHELL_PACKETS_H_

#include <string>

#include "src/trace_redaction/trace_redaction_framework.h"

namespace perfetto::trace_redaction {

// Removes shell packets: packets whose only fields are kShellPacketFields
// (trusted uid/pid, sequence id, timestamp and its clock id). Such packets
// carry no content and cannot affect how any other packet is decoded, so they
// are safe to remove in any trace type. They are typically the remnants of
// packets whose payload was removed by earlier primitives (e.g.
// BroadphasePacketFilter).
//
// Packets with any other field are kept, including metadata that does affect
// trace processing: incremental state (interned_data, trace_packet_defaults,
// sequence_flags, incremental_state_cleared), first_packet_on_sequence,
// previous_packet_dropped (data loss) and machine_id.
//
// Example:
//
//   Before:
//     packet {                      # shell remnant: dropped
//       trusted_uid: 9999
//       timestamp: 4804032990072
//       trusted_packet_sequence_id: 1
//       timestamp_clock_id: 6
//     }
//     packet {                      # carries data-loss marker: kept
//       trusted_uid: 9999
//       trusted_packet_sequence_id: 2
//       previous_packet_dropped: true
//     }
//
//   After:
//     packet {
//       trusted_uid: 9999
//       trusted_packet_sequence_id: 2
//       previous_packet_dropped: true
//     }
class PruneShellPackets final : public TransformPrimitive {
 public:
  base::Status Transform(const Context& context,
                         std::string* packet) const override;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_PRUNE_SHELL_PACKETS_H_
