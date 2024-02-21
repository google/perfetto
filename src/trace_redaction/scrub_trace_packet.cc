/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_redaction/scrub_trace_packet.h"

namespace perfetto::trace_redaction {
// The TracePacket message has a simple structure. At its core its one sub
// message (e.g. ProcessTree) and some additional context (e.g. timestamp).
// This makes the per-packet check binary - does it contain one of the
// allow-listed messages?
//
// This transform will be called P times where P is the number of packet in the
// trace.
//
// There are A packet types in the allow-list. The allow-list in a set with logA
// look up. Since the allow-list is relatively small and constant in size,
// allow-list can be considered constant.
//
// There are at most F fields where F is the max number of concurrent fields in
// a trace packet. Given the limit, this can be considered constant.
//
// All together, this implementation can be considered linear in relation to the
// trace size.
base::Status ScrubTracePacket::Transform(const Context&,
                                         std::string* packet) const {
  protozero::ProtoDecoder d(*packet);

  // A packet should only have one data type (proto oneof), but there are other
  // values in the packet (e.g. timestamp). If one field is in the allowlist,
  // then allow the whole trace packet.
  for (auto f = d.ReadField(); f.valid(); f = d.ReadField()) {
    if (allow_list_.count(f.id()) != 0) {
      return base::OkStatus();
    }
  }

  packet->clear();
  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
