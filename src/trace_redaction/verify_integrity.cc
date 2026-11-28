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

#include "perfetto/base/compiler.h"
#include "perfetto/ext/base/status_macros.h"

#include "protos/perfetto/common/trace_stats.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {

base::Status VerifyIntegrity::Collect(
    const protos::pbzero::TracePacket::Decoder& packet,
    Context*) const {
  if (PERFETTO_UNLIKELY(packet.has_compressed_packets() ||
                        packet.has_zstd_compressed_packets())) {
    return base::ErrStatus(
        "VerifyIntegrity: compressed packets found; decompress the trace "
        "first.");
  }

  // App-uid and uid-less packets are filtered or dropped during Transform
  // (FilterUntrustedAppPackets, PruneMissingUidPackets, app_packet_mask); skip
  // system payload checks here so an app cannot fail the whole trace.
  if (GetWriterType(packet) != WriterType::kSystem) {
    return base::OkStatus();
  }

  if (packet.has_ftrace_events()) {
    RETURN_IF_ERROR(OnFtraceEvents(packet.ftrace_events()));
  }

  // If there is a process tree, there should be a timestamp on the packet. This
  // is the only way to know when the process tree was collected.
  if (PERFETTO_UNLIKELY(packet.has_process_tree() && !packet.has_timestamp())) {
    return base::ErrStatus(
        "VerifyIntegrity: missing fields (TracePacket::kProcessTree + "
        "TracePacket::kTimestamp).");
  }

  // If there are a process stats, there should be a timestamp on the packet.
  // This is the only way to know when the stats were collected.
  if (PERFETTO_UNLIKELY(packet.has_process_stats() &&
                        !packet.has_timestamp())) {
    return base::ErrStatus(
        "VerifyIntegrity: missing fields (TracePacket::kProcessStats + "
        "TracePacket::kTimestamp).");
  }

  if (packet.has_trace_stats()) {
    RETURN_IF_ERROR(OnTraceStats(packet.trace_stats()));
  }

  return base::OkStatus();
}

base::Status VerifyIntegrity::OnFtraceEvents(
    const protozero::ConstBytes bytes) const {
  protos::pbzero::FtraceEventBundle::Decoder events(bytes);

  // Any ftrace lost events should cause the trace to be dropped:
  // protos/perfetto/trace/ftrace/ftrace_event_bundle.proto
  if (PERFETTO_UNLIKELY(events.has_lost_events())) {
    return base::ErrStatus(
        "VerifyIntegrity: detected FtraceEventBundle error.");
  }

  // The other clocks in ftrace are only used on very old kernel versions. No
  // device with V should have such an old version. As a failsafe though,
  // check that the ftrace_clock field is unset to ensure no invalid
  // timestamps get by.
  if (PERFETTO_UNLIKELY(events.has_ftrace_clock())) {
    return base::ErrStatus(
        "VerifyIntegrity: unexpected field (FtraceEventBundle::kFtraceClock).");
  }

  // Every ftrace event bundle should have a CPU field. This is necessary for
  // switch/waking redaction to work.
  if (PERFETTO_UNLIKELY(!events.has_cpu() ||
                        events.cpu() >= SystemInfo::kMaxCpus)) {
    return base::ErrStatus(
        "VerifyIntegrity: missing or out-of-range field "
        "(FtraceEventBundle::kCpu).");
  }

  // Any ftrace errors should cause the trace to be dropped:
  // protos/perfetto/trace/ftrace/ftrace_event_bundle.proto
  if (PERFETTO_UNLIKELY(events.has_error())) {
    return base::ErrStatus("VerifyIntegrity: detected FtraceEvent errors.");
  }

  for (auto it = events.event(); it; ++it) {
    RETURN_IF_ERROR(OnFtraceEvent(*it));
  }

  return base::OkStatus();
}

base::Status VerifyIntegrity::OnFtraceEvent(
    const protozero::ConstBytes bytes) const {
  protos::pbzero::FtraceEvent::Decoder event(bytes);

  if (PERFETTO_UNLIKELY(!event.has_timestamp())) {
    return base::ErrStatus(
        "VerifyIntegrity: missing field (FtraceEvent::kTimestamp).");
  }

  if (PERFETTO_UNLIKELY(!event.has_pid())) {
    return base::ErrStatus(
        "VerifyIntegrity: missing field (FtraceEvent::kPid).");
  }

  return base::OkStatus();
}

base::Status VerifyIntegrity::OnTraceStats(
    const protozero::ConstBytes bytes) const {
  protos::pbzero::TraceStats::Decoder trace_stats(bytes);

  if (PERFETTO_UNLIKELY(trace_stats.has_flushes_failed() &&
                        trace_stats.flushes_failed())) {
    return base::ErrStatus("VerifyIntegrity: detected TraceStats flush fails.");
  }

  if (PERFETTO_UNLIKELY(trace_stats.has_final_flush_outcome() &&
                        trace_stats.final_flush_outcome() ==
                            protos::pbzero::TraceStats::FINAL_FLUSH_FAILED)) {
    return base::ErrStatus(
        "VerifyIntegrity: TraceStats final_flush_outcome is "
        "FINAL_FLUSH_FAILED.");
  }

  for (auto it = trace_stats.buffer_stats(); it; ++it) {
    RETURN_IF_ERROR(OnBufferStats(*it));
  }

  return base::OkStatus();
}

base::Status VerifyIntegrity::OnBufferStats(
    const protozero::ConstBytes bytes) const {
  protos::pbzero::TraceStats::BufferStats::Decoder stats(bytes);

  if (PERFETTO_UNLIKELY(stats.has_patches_failed() && stats.patches_failed())) {
    return base::ErrStatus(
        "VerifyIntegrity: detected BufferStats patch fails.");
  }

  if (PERFETTO_UNLIKELY(stats.has_abi_violations() && stats.abi_violations())) {
    return base::ErrStatus(
        "VerifyIntegrity: detected BufferStats abi violations.");
  }

  auto has_loss = stats.has_trace_writer_packet_loss();
  auto value = stats.trace_writer_packet_loss();

  if (PERFETTO_UNLIKELY(has_loss && value)) {
    return base::ErrStatus(
        "VerifyIntegrity: detected BufferStats writer packet loss.");
  }

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
