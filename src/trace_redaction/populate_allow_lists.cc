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

#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {

base::Status PopulateAllowlists::Build(Context* context) const {
  if (!context->trace_packet_allow_list.empty()) {
    return base::ErrStatus("PopulateAllowlists: allow-list should be empty.");
  }

  // TRACE PACKET NOTES
  //
  //    protos::pbzero::TracePacket::kAndroidSystemPropertyFieldNumber
  //
  //      AndroidSystemProperty exposes a key-value pair structure with no
  //      constraints around keys or values, making fine-grain redaction
  //      difficult. Because this packet's value has no measurable, the safest
  //      option to drop the whole packet.

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
      protos::pbzero::TracePacket::kSynchronizationMarkerFieldNumber,
      protos::pbzero::TracePacket::kFtraceEventsFieldNumber,

      // Keep the package list. There are some metrics and stdlib queries that
      // depend on the package list.
      protos::pbzero::TracePacket::kPackagesListFieldNumber,
  };

  context->ftrace_packet_allow_list = {
      protos::pbzero::FtraceEvent::kSchedSwitchFieldNumber,
      protos::pbzero::FtraceEvent::kCpuFrequencyFieldNumber,
      protos::pbzero::FtraceEvent::kCpuIdleFieldNumber,
      protos::pbzero::FtraceEvent::kSchedBlockedReasonFieldNumber,
      protos::pbzero::FtraceEvent::kSchedWakingFieldNumber,
      protos::pbzero::FtraceEvent::kSuspendResumeFieldNumber,
      protos::pbzero::FtraceEvent::kTaskNewtaskFieldNumber,
      protos::pbzero::FtraceEvent::kTaskRenameFieldNumber,
      protos::pbzero::FtraceEvent::kSchedProcessFreeFieldNumber,
      protos::pbzero::FtraceEvent::kRssStatFieldNumber,
      protos::pbzero::FtraceEvent::kIonHeapShrinkFieldNumber,
      protos::pbzero::FtraceEvent::kIonHeapGrowFieldNumber,
      protos::pbzero::FtraceEvent::kIonStatFieldNumber,
      protos::pbzero::FtraceEvent::kIonBufferCreateFieldNumber,
      protos::pbzero::FtraceEvent::kIonBufferDestroyFieldNumber,
      protos::pbzero::FtraceEvent::kDmaHeapStatFieldNumber,
      protos::pbzero::FtraceEvent::kRssStatThrottledFieldNumber,
      protos::pbzero::FtraceEvent::kPrintFieldNumber,
  };

  // TODO: Some ftrace fields should be retained, but they carry too much risk
  // without additional redaction. This list should be configured in a build
  // primitive so that they can be optionally included.
  //
  // protos::pbzero::FtraceEvent::kPrintFieldNumber,
  //
  // TODO: Some fields will create new packets (e.g. binder calls may create
  // new spans. This is currently not supported (generated packets still
  // need to be redacted).
  //
  // protos::pbzero::FtraceEvent::kBinderTransactionFieldNumber,
  // protos::pbzero::FtraceEvent::kBinderTransactionReceivedFieldNumber,
  // protos::pbzero::FtraceEvent::kBinderSetPriorityFieldNumber,
  // protos::pbzero::FtraceEvent::kBinderLockedFieldNumber,
  // protos::pbzero::FtraceEvent::kBinderUnlockFieldNumber,

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
