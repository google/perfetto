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
#include "protos/third_party/android/frameworks/native/tracing/frameworks_native_trace_packet.pbzero.h"

namespace perfetto::trace_redaction {
namespace {

using TracePacket = protos::pbzero::TracePacket;

// Top-level fields outside `TracePacket`'s `oneof data` (timestamps, uids,
// sequence ids, etc.).
void AllowTracePacketNonDataFields(Context::TracePacketMask* packet_mask) {
  for (uint32_t id : kTracePacketNonDataFields) {
    packet_mask->set(id);
  }
}

// Device and tracing-session metadata.
void AllowSessionMetadata(Context::TracePacketMask* packet_mask) {
  packet_mask->set(TracePacket::kClockSnapshotFieldNumber);
  packet_mask->set(TracePacket::kServiceEventFieldNumber);
  packet_mask->set(TracePacket::kSynchronizationMarkerFieldNumber);
  packet_mask->set(TracePacket::kSystemInfoFieldNumber);
  packet_mask->set(TracePacket::kTraceConfigFieldNumber);
  packet_mask->set(TracePacket::kTraceStatsFieldNumber);
  packet_mask->set(TracePacket::kTraceUuidFieldNumber);
  packet_mask->set(TracePacket::kTriggerFieldNumber);
}

// Per-package and per-process attribution tables (narrow-phase transforms prune
// rows to the target package).
void AllowProcessAndPackageTables(Context::TracePacketMask* packet_mask) {
  packet_mask->set(TracePacket::kPackagesListFieldNumber);
  packet_mask->set(TracePacket::kProcessStatsFieldNumber);
  packet_mask->set(TracePacket::kProcessTreeFieldNumber);
}

}  // namespace

base::Status PopulateAllowlists::Build(Context* context) const {
  // App-uid packets (`WriterType::kApp`) may only carry non-data fields (plus
  // explicitly allowlisted app payloads), never spoofed system payloads.
  auto* app_packet_mask = &context->app_packet_mask;
  AllowTracePacketNonDataFields(app_packet_mask);

  // System packet mask includes everything allowed on app packets, plus session
  // metadata, attribution tables, and system-trace payloads.
  auto* system_packet_mask = &context->packet_mask;
  *system_packet_mask = *app_packet_mask;
  AllowSessionMetadata(system_packet_mask);
  AllowProcessAndPackageTables(system_packet_mask);

  // System-trace payloads (every field here must also be handled by a
  // message-focused transform).
  system_packet_mask->set(
      com::android::internal::pbzero::FrameworksNativeTracePacket::
          kFrameTimelineEventFieldNumber);
  system_packet_mask->set(TracePacket::kCpuInfoFieldNumber);
  system_packet_mask->set(TracePacket::kFtraceEventsFieldNumber);
  system_packet_mask->set(TracePacket::kInitialDisplayStateFieldNumber);
  system_packet_mask->set(TracePacket::kPerfSampleFieldNumber);
  system_packet_mask->set(TracePacket::kSysStatsFieldNumber);

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
  //    TODO(vaage): kSchedBlockedReasonFieldNumber contains two pids, an outer
  //    and inner pid. A primitive is needed to further redact these events.

  auto& ftrace_masks = context->ftrace_mask;

  ftrace_masks.set(protos::pbzero::FtraceEvent::kCommonFlagsFieldNumber);
  ftrace_masks.set(protos::pbzero::FtraceEvent::kCpuFrequencyFieldNumber);
  ftrace_masks.set(protos::pbzero::FtraceEvent::kCpuIdleFieldNumber);
  ftrace_masks.set(protos::pbzero::FtraceEvent::kOomScoreAdjUpdateFieldNumber);
  ftrace_masks.set(protos::pbzero::FtraceEvent::kPidFieldNumber);
  ftrace_masks.set(protos::pbzero::FtraceEvent::kPrintFieldNumber);
  ftrace_masks.set(protos::pbzero::FtraceEvent::kRssStatFieldNumber);
  ftrace_masks.set(protos::pbzero::FtraceEvent::kRssStatThrottledFieldNumber);
  ftrace_masks.set(protos::pbzero::FtraceEvent::kSchedBlockedReasonFieldNumber);
  ftrace_masks.set(protos::pbzero::FtraceEvent::kSchedProcessFreeFieldNumber);
  ftrace_masks.set(protos::pbzero::FtraceEvent::kSchedSwitchFieldNumber);
  ftrace_masks.set(protos::pbzero::FtraceEvent::kSchedWakingFieldNumber);
  ftrace_masks.set(protos::pbzero::FtraceEvent::kTaskNewtaskFieldNumber);
  ftrace_masks.set(protos::pbzero::FtraceEvent::kTaskRenameFieldNumber);
  ftrace_masks.set(protos::pbzero::FtraceEvent::kTimestampFieldNumber);

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
