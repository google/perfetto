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

#include "src/trace_redaction/prune_missing_uid_packets.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {
namespace {

using TracePacket = protos::pbzero::TracePacket;

}  // namespace

base::Status PruneMissingUidPackets::Transform(const Context& context,
                                               std::string* packet) const {
  if (GetWriterType(TracePacket::Decoder(*packet)) != WriterType::kMissingUid) {
    return base::OkStatus();
  }

  for (const auto& filter : allow_list_) {
    if (filter->Includes(context, *packet)) {
      return base::OkStatus();
    }
  }

  packet->clear();
  context.stats.missing_uid_packets_dropped++;
  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
