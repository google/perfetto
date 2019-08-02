/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef TOOLS_TRACE_TO_TEXT_PROFILE_VISITOR_H_
#define TOOLS_TRACE_TO_TEXT_PROFILE_VISITOR_H_

#include <vector>

#include "perfetto/base/logging.h"

#include "tools/trace_to_text/utils.h"

#include "perfetto/trace/interned_data/interned_data.pb.h"
#include "perfetto/trace/profiling/profile_common.pb.h"
#include "perfetto/trace/profiling/profile_packet.pb.h"
#include "perfetto/trace/trace_packet.pb.h"

namespace perfetto {
namespace trace_to_text {

class ProfileVisitor {
 public:
  bool Visit(const std::vector<protos::ProfilePacket>&,
             const std::vector<protos::InternedData>&);
  virtual bool AddInternedString(
      const protos::InternedString& interned_string) = 0;
  virtual bool AddCallstack(const protos::Callstack& callstack) = 0;
  virtual bool AddMapping(const protos::Mapping& mapping) = 0;
  virtual bool AddFrame(const protos::Frame& frame) = 0;
  virtual ~ProfileVisitor();
};

template <typename F>
bool VisitCompletePacket(std::istream* input, F fn) {
  std::map<uint32_t, std::vector<protos::ProfilePacket>>
      rolling_profile_packets_by_seq;
  std::map<uint32_t, std::vector<protos::InternedData>>
      rolling_interned_data_by_seq;
  bool success = true;
  ForEachPacketInTrace(input, [&rolling_profile_packets_by_seq,
                               &rolling_interned_data_by_seq, &success,
                               &fn](const protos::TracePacket& packet) {
    uint32_t seq_id = packet.trusted_packet_sequence_id();
    if (packet.has_interned_data())
      rolling_interned_data_by_seq[seq_id].emplace_back(packet.interned_data());

    if (!packet.has_profile_packet())
      return;

    rolling_profile_packets_by_seq[seq_id].emplace_back(
        packet.profile_packet());

    const std::vector<protos::InternedData>& rolling_interned_data =
        rolling_interned_data_by_seq[seq_id];
    const std::vector<protos::ProfilePacket>& rolling_profile_packets =
        rolling_profile_packets_by_seq[seq_id];

    if (!packet.profile_packet().continued()) {
      for (size_t i = 1; i < rolling_profile_packets.size(); ++i) {
        // Ensure we are not missing a chunk.
        if (rolling_profile_packets[i - 1].index() + 1 !=
            rolling_profile_packets[i].index()) {
          success = false;
          return;
        }
      }
      if (!fn(seq_id, rolling_profile_packets, rolling_interned_data))
        success = false;

      // We do not clear rolling_interned_data, as it is globally scoped.
      rolling_profile_packets_by_seq.erase(seq_id);
    }
  });

  if (!rolling_profile_packets_by_seq.empty()) {
    PERFETTO_ELOG("WARNING: Truncated heap dump.");
    return false;
  }
  return success;
}

}  // namespace trace_to_text
}  // namespace perfetto

#endif  // TOOLS_TRACE_TO_TEXT_PROFILE_VISITOR_H_
