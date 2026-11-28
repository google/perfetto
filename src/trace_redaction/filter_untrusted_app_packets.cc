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

#include "src/trace_redaction/filter_untrusted_app_packets.h"

#include "perfetto/base/logging.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {
namespace {

using TracePacket = protos::pbzero::TracePacket;

}  // namespace

base::Status FilterUntrustedAppPackets::Transform(const Context& context,
                                                  std::string* packet) const {
  // Validated once before Transform() by PackageValidation.
  PERFETTO_DCHECK(context.package_uid.has_value());

  if (context.IsOtherAppPacket(TracePacket::Decoder(*packet))) {
    packet->clear();
    context.stats.untrusted_app_packets_dropped++;
  }
  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
