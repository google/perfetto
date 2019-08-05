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

#include "perfetto/trace/trace.pb.h"
#include "perfetto/trace/trace_packet.pb.h"

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
}  // namespace

bool ProfileVisitor::Visit(const std::vector<ProfilePacket>& packet_fragments,
                           const std::vector<InternedData>& interned_data) {
  for (const ProfilePacket& packet : packet_fragments) {
    for (const InternedString& interned_string : packet.strings())
      if (!AddInternedString(interned_string))
        return false;
  }
  for (const InternedData& data : interned_data) {
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
    for (const ProfiledFrameSymbols& pfs : data.profiled_frame_symbols())
      if (!AddProfiledFrameSymbols(pfs))
        return false;
  }

  for (const ProfilePacket& packet : packet_fragments) {
    for (const Callstack& callstack : packet.callstacks())
      if (!AddCallstack(callstack))
        return false;
  }
  for (const InternedData& data : interned_data) {
    for (const Callstack& callstack : data.callstacks())
      if (!AddCallstack(callstack))
        return false;
  }

  for (const ProfilePacket& packet : packet_fragments) {
    for (const Mapping& mapping : packet.mappings())
      if (!AddMapping(mapping))
        return false;
  }
  for (const InternedData& data : interned_data) {
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
  for (const InternedData& data : interned_data) {
    for (const Frame& frame : data.frames()) {
      if (!AddFrame(frame))
        return false;
    }
  }
  return true;
}

ProfileVisitor::~ProfileVisitor() = default;

}  // namespace trace_to_text
}  // namespace perfetto
