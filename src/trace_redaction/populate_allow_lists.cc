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
  // These fields are top-level fields that outside the "oneof data" field.
  std::initializer_list<uint32_t> required_trace_fields = {

      protos::pbzero::TracePacket::kTimestampFieldNumber,
      protos::pbzero::TracePacket::kTimestampClockIdFieldNumber,
      protos::pbzero::TracePacket::kTrustedUidFieldNumber,
      protos::pbzero::TracePacket::kTrustedPacketSequenceIdFieldNumber,
      protos::pbzero::TracePacket::kTrustedPidFieldNumber,
      protos::pbzero::TracePacket::kInternedDataFieldNumber,
      protos::pbzero::TracePacket::kSequenceFlagsFieldNumber,

      // DEPRECATED. Moved to SequenceFlags::SEQ_INCREMENTAL_STATE_CLEARED. So
      // there is no reason to include it.
      //
      // protos::pbzero::TracePacket::incremental_state_cleared

      protos::pbzero::TracePacket::kTracePacketDefaultsFieldNumber,
      protos::pbzero::TracePacket::kPreviousPacketDroppedFieldNumber,
      protos::pbzero::TracePacket::kFirstPacketOnSequenceFieldNumber,
      protos::pbzero::TracePacket::kMachineIdFieldNumber,
  };

  for (auto item : required_trace_fields) {
    context->trace_packet_allow_list.insert(item);
  }

  // TRACE PACKET NOTES
  //
  //    protos::pbzero::TracePacket::kAndroidSystemPropertyFieldNumber
  //
  //      AndroidSystemProperty exposes a key-value pair structure with no
  //      constraints around keys or values, making fine-grain redaction
  //      difficult. Because this packet's value has no measurable, the safest
  //      option to drop the whole packet.
  std::initializer_list<uint32_t> trace_packets = {
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

  for (auto item : trace_packets) {
    context->trace_packet_allow_list.insert(item);
  }

  // FTRACE EVENT NOTES
  //
  //    Dma events (kDmaHeapStatFieldNumber) are global events and are not
  //    emitted within a process context (they are centrally allocated by the
  //    HAL process). We drop them for now as we don't have the required
  //    attribution info in the trace.
  //
  //    ION events (e.g. kIonBufferCreateFieldNumber, kIonHeapGrowFieldNumber,
  //    etc.) are global events are not emitted within a process context (they
  //    are centrally allocated by the HAL process). We drop them for now as we
  //    don't have the required attribution info in the trace.
  //
  //    TODO(vaage): The allowed rss stat events (i.e. kRssStatFieldNumber,
  //    kRssStatThrottledFieldNumber) are process-scoped. It is non-trivial to
  //    merge events, so all events outside of the target package should be
  //    dropped.
  //
  //    TODO(vaage): kSchedBlockedReasonFieldNumber contains two pids, an outer
  //    and inner pid. A primitive is needed to further redact these events.

  std::initializer_list<uint32_t> ftrace_events = {
      protos::pbzero::FtraceEvent::kCpuFrequencyFieldNumber,
      protos::pbzero::FtraceEvent::kCpuIdleFieldNumber,
      protos::pbzero::FtraceEvent::kPrintFieldNumber,
      protos::pbzero::FtraceEvent::kRssStatFieldNumber,
      protos::pbzero::FtraceEvent::kRssStatThrottledFieldNumber,
      protos::pbzero::FtraceEvent::kSchedBlockedReasonFieldNumber,
      protos::pbzero::FtraceEvent::kSchedProcessFreeFieldNumber,
      protos::pbzero::FtraceEvent::kSchedSwitchFieldNumber,
      protos::pbzero::FtraceEvent::kSchedWakingFieldNumber,
      protos::pbzero::FtraceEvent::kTaskNewtaskFieldNumber,
      protos::pbzero::FtraceEvent::kTaskRenameFieldNumber,
  };

  for (auto item : ftrace_events) {
    context->ftrace_packet_allow_list.insert(item);
  }

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
