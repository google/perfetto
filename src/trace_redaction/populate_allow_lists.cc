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

#include "src/trace_redaction/populate_allow_lists.h"

#include "perfetto/base/status.h"
#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {

base::Status PopulateAllowlists::Build(Context* context) const {
  if (!context->trace_packet_allow_list.empty()) {
    return base::ErrStatus("Trace packet allow-list should be empty.");
  }

  context->trace_packet_allow_list = {
      protos::pbzero::TracePacket::kProcessTreeFieldNumber,
      protos::pbzero::TracePacket::kProcessStatsFieldNumber,
      protos::pbzero::TracePacket::kClockSnapshotFieldNumber,
      protos::pbzero::TracePacket::kSysStatsFieldNumber,
      protos::pbzero::TracePacket::kTraceConfigFieldNumber,
      protos::pbzero::TracePacket::kTraceStatsFieldNumber,
      protos::pbzero::TracePacket::kSystemInfoFieldNumber,
      protos::pbzero::TracePacket::kTriggerFieldNumber,
      protos::pbzero::TracePacket::kCpuInfoFieldNumber,
      protos::pbzero::TracePacket::kServiceEventFieldNumber,
      protos::pbzero::TracePacket::kInitialDisplayStateFieldNumber,
      protos::pbzero::TracePacket::kFrameTimelineEventFieldNumber,
      protos::pbzero::TracePacket::kAndroidSystemPropertyFieldNumber,
      protos::pbzero::TracePacket::kSynchronizationMarkerFieldNumber,
      protos::pbzero::TracePacket::kFtraceEventsFieldNumber,
  };

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
