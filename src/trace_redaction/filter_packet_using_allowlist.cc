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

#include <string>

#include "perfetto/base/status.h"
#include "perfetto/protozero/field.h"
#include "src/trace_redaction/filter_packet_using_allowlist.h"
#include "src/trace_redaction/trace_redaction_framework.h"

namespace perfetto::trace_redaction {

base::Status FilterPacketUsingAllowlist::VerifyContext(
    const Context& context) const {
  if (context.trace_packet_allow_list.empty()) {
    return base::ErrStatus("FilterPacketUsingAllowlist: missing allow-list.");
  }

  return base::OkStatus();
}

bool FilterPacketUsingAllowlist::KeepPacket(const Context& context,
                                            const std::string& bytes) const {
  PERFETTO_DCHECK(!context.trace_packet_allow_list.empty());

  const auto& allow_list = context.trace_packet_allow_list;

  protozero::ProtoDecoder decoder(bytes);

  // A packet should only have one data type (proto oneof), but there are other
  // values in the packet (e.g. timestamp). If one field is in the allowlist,
  // then allow the whole trace packet.
  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    if (allow_list.count(field.id()) != 0) {
      return true;
    }
  }

  return false;
}

}  // namespace perfetto::trace_redaction
