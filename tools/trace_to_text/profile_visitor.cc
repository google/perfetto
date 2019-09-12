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

#include "tools/trace_to_text/profile_visitor.h"

#include <unordered_map>
#include "protos/perfetto/trace/trace.pb.h"
#include "protos/perfetto/trace/trace_packet.pb.h"

#include "perfetto/ext/base/string_splitter.h"

namespace perfetto {
namespace trace_to_text {

namespace {
using ::perfetto::protos::Callstack;
using ::perfetto::protos::Frame;
using ::perfetto::protos::InternedData;
using ::perfetto::protos::InternedString;
using ::perfetto::protos::Mapping;
using ::perfetto::protos::ProfiledFrameSymbols;
using ::perfetto::protos::ProfilePacket;

struct ProfilePackets {
  uint32_t seq_id;
  std::vector<protos::ProfilePacket> packets;
};

bool IsPacketIndexContiguous(
    const std::vector<perfetto::protos::ProfilePacket>& packets) {
  for (size_t i = 1; i < packets.size(); ++i) {
    // Ensure we are not missing a chunk.
    if (packets[i - 1].index() + 1 != packets[i].index()) {
      return false;
    }
  }
  return true;
}
}  // namespace

bool ProfileVisitor::Visit(
    const std::vector<protos::ProfilePacket>& packet_fragments,
    const SequencedBundle& bundle) {
  for (const ProfilePacket& packet : packet_fragments) {
    for (const InternedString& interned_string : packet.strings())
      if (!AddInternedString(interned_string))
        return false;
  }
  for (const InternedData& data : bundle.interned_data) {
    for (const InternedString& interned_string : data.build_ids())
      if (!AddInternedString(interned_string))
        return false;
    for (const InternedString& interned_string : data.mapping_paths())
      if (!AddInternedString(interned_string))
        return false;
    for (const InternedString& interned_string : data.function_names())
      if (!AddInternedString(interned_string))
        return false;
    for (const InternedString& interned_string : data.source_paths())
      if (!AddInternedString(interned_string))
        return false;
    // TODO (140860736): This should be outside the interned section.
    for (const ProfiledFrameSymbols& pfs : data.profiled_frame_symbols())
      if (!AddProfiledFrameSymbols(pfs))
        return false;
  }
  for (const ProfiledFrameSymbols& pfs : bundle.symbols)
    if (!AddProfiledFrameSymbols(pfs))
      return false;

  for (const ProfilePacket& packet : packet_fragments) {
    for (const Callstack& callstack : packet.callstacks())
      if (!AddCallstack(callstack))
        return false;
  }
  for (const InternedData& data : bundle.interned_data) {
    for (const Callstack& callstack : data.callstacks())
      if (!AddCallstack(callstack))
        return false;
  }

  for (const ProfilePacket& packet : packet_fragments) {
    for (const Mapping& mapping : packet.mappings())
      if (!AddMapping(mapping))
        return false;
  }
  for (const InternedData& data : bundle.interned_data) {
    for (const Mapping& callstack : data.mappings()) {
      if (!AddMapping(callstack))
        return false;
    }
  }

  for (const ProfilePacket& packet : packet_fragments) {
    for (const Frame& frame : packet.frames()) {
      if (!AddFrame(frame))
        return false;
    }
  }
  for (const InternedData& data : bundle.interned_data) {
    for (const Frame& frame : data.frames()) {
      if (!AddFrame(frame))
        return false;
    }
  }
  return true;
}

ProfileVisitor::~ProfileVisitor() = default;

bool VisitCompletePacket(
    std::istream* input,
    const std::function<bool(uint32_t,
                             const std::vector<protos::ProfilePacket>&,
                             const SequencedBundle&)>& fn) {
  // Rolling profile packets per seq id. Cleared on finalization.
  std::unordered_map<uint32_t, std::vector<protos::ProfilePacket>>
      rolling_profile_packets_by_seq;
  std::vector<ProfilePackets> complete_profile_packets;
  // Append-only interned data and symbols by seq id
  std::unordered_map<uint32_t, SequencedBundle> bundle_by_seq;
  ForEachPacketInTrace(input, [&rolling_profile_packets_by_seq,
                               &complete_profile_packets, &bundle_by_seq](
                                  const protos::TracePacket& packet) {
    uint32_t seq_id = packet.trusted_packet_sequence_id();
    if (packet.has_interned_data()) {
      bundle_by_seq[seq_id].interned_data.emplace_back(packet.interned_data());
    }
    if (packet.has_appended_data()) {
      std::copy(packet.appended_data().profiled_frame_symbols().cbegin(),
                packet.appended_data().profiled_frame_symbols().cend(),
                std::back_inserter(bundle_by_seq[seq_id].symbols));
    }

    if (packet.has_profile_packet()) {
      std::vector<protos::ProfilePacket>& rolling_profile_packets =
          rolling_profile_packets_by_seq[seq_id];
      rolling_profile_packets.emplace_back(packet.profile_packet());

      if (!packet.profile_packet().continued()) {
        if (IsPacketIndexContiguous(rolling_profile_packets)) {
          complete_profile_packets.push_back({seq_id, rolling_profile_packets});
          rolling_profile_packets_by_seq.erase(seq_id);
        }
      }
    }
  });

  bool success = true;
  for (const auto& packets : complete_profile_packets) {
    success &=
        fn(packets.seq_id, packets.packets, bundle_by_seq[packets.seq_id]);
  }
  if (!rolling_profile_packets_by_seq.empty()) {
    PERFETTO_ELOG("WARNING: Truncated heap dump.");
    return false;
  }
  return success;
}
}  // namespace trace_to_text
}  // namespace perfetto
