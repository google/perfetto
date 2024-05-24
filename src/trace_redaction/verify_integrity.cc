/*
 * Copyright (C) 2024 The Android Open Source Projectf
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

#include "src/trace_redaction/verify_integrity.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto::trace_redaction {
namespace {
// All constants are from
// "system/core/libcutils/include/private/android_filesystem_config.h"
//
// AID 1000 == system (those are probably frame_timeline packets you will see
// those on) AID 9999 is nobody == traced/traced_probes
constexpr int32_t kAidSystem = 1000;
constexpr int32_t kAidNobody = 9999;
}  // namespace

base::Status VerifyIntegrity::Collect(
    const protos::pbzero::TracePacket::Decoder& packet,
    Context*) const {
  if (!packet.has_trusted_uid()) {
    return base::ErrStatus(
        "VerifyIntegrity: missing field (TracePacket.trusted_uid).");
  }

  if (packet.trusted_uid() != kAidSystem &&
      packet.trusted_uid() != kAidNobody) {
    return base::ErrStatus(
        "VerifyIntegrity: invalid field value (TracePacket.trusted_uid).");
  }

  if (packet.has_ftrace_events()) {
    protos::pbzero::FtraceEventBundle::Decoder ftrace_events(
        packet.ftrace_events());

    // The other clocks in ftrace are only used on very old kernel versions. No
    // device with V should have such an old version. As a failsafe though,
    // check that the ftrace_clock field is unset to ensure no invalid
    // timestamps get by.
    if (ftrace_events.has_ftrace_clock()) {
      return base::ErrStatus(
          "VerifyIntegrity: unexpected field "
          "(FtraceEventBundle::kFtraceClockFieldNumber).");
    }

    // Every ftrace event bundle should have a CPU field. This is necessary for
    // switch/waking redaction to work.
    if (!ftrace_events.has_cpu()) {
      return base::ErrStatus(
          "VerifyIntegrity: missing field "
          "(FtraceEventBundle::kCpuFieldNumber).");
    }

    RETURN_IF_ERROR(VerifyFtraceEventsTime(ftrace_events));
  }

  return base::OkStatus();
}

base::Status VerifyIntegrity::VerifyFtraceEventsTime(
    const protos::pbzero::FtraceEventBundle::Decoder& bundle) const {
  // If a bundle has ftrace events, the events will contain the time stamps.
  // However, there are no ftrace events, the timestamp will be in the bundle.
  if (!bundle.has_event() && !bundle.has_ftrace_timestamp()) {
    return base::ErrStatus(
        "VerifyIntegrity: missing field "
        "(FtraceEventBundle::kFtraceTimestampFieldNumber).");
  }

  for (auto event_buffer = bundle.event(); event_buffer; ++event_buffer) {
    protos::pbzero::FtraceEvent::Decoder event(*event_buffer);

    if (!protozero::ProtoDecoder(*event_buffer)
             .FindField(protos::pbzero::FtraceEvent::kTimestampFieldNumber)
             .valid()) {
      return base::ErrStatus(
          "VerifyIntegrity: missing field "
          "(FtraceEvent::kTimestampFieldNumber)");
    }
  }

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
