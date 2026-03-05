/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/importers/proto/linux_probes_module.h"

#include <cstdint>
#include <optional>

#include "perfetto/base/logging.h"
#include "perfetto/protozero/field.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "protos/perfetto/trace/linux/journald_event.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/proto/blob_packet_writer.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

using perfetto::protos::pbzero::TracePacket;

LinuxProbesModule::LinuxProbesModule(ProtoImporterModuleContext* module_context,
                                     TraceProcessorContext* context)
    : ProtoImporterModule(module_context), parser_(context), context_(context) {
  RegisterForField(TracePacket::kJournaldEventFieldNumber);
}

ModuleResult LinuxProbesModule::TokenizePacket(
    const protos::pbzero::TracePacket_Decoder&,
    TraceBlobView* packet,
    int64_t /*packet_timestamp*/,
    RefPtr<PacketSequenceStateGeneration> state,
    uint32_t field_id) {
  if (field_id != TracePacket::kJournaldEventFieldNumber) {
    return ModuleResult::Ignored();
  }

  protos::pbzero::TracePacket::Decoder decoder(packet->data(),
                                               packet->length());
  auto journald_bytes = decoder.journald_event();
  protos::pbzero::JournaldEventPacket::Decoder pkt(journald_bytes);

  for (auto it = pkt.events(); it; ++it) {
    protos::pbzero::JournaldEventPacket::JournaldEvent::Decoder evt(*it);
    if (!evt.has_timestamp_us()) {
      continue;
    }
    int64_t ts_ns = static_cast<int64_t>(evt.timestamp_us()) * 1000;
    std::optional<int64_t> trace_ts = context_->clock_tracker->ToTraceTime(
        ClockId::Machine(protos::pbzero::BUILTIN_CLOCK_REALTIME), ts_ns);
    if (!trace_ts.has_value()) {
      continue;
    }
    int64_t actual_ts = *trace_ts;

    TraceBlobView tbv =
        context_->blob_packet_writer->WritePacket([&](auto* data_packet) {
          data_packet->set_timestamp(static_cast<uint64_t>(actual_ts));
          auto* jpkt = data_packet->set_journald_event();
          auto* jevt = jpkt->add_events();
          jevt->set_timestamp_us(evt.timestamp_us());
          if (evt.has_pid())
            jevt->set_pid(evt.pid());
          if (evt.has_tid())
            jevt->set_tid(evt.tid());
          if (evt.has_uid())
            jevt->set_uid(evt.uid());
          if (evt.has_gid())
            jevt->set_gid(evt.gid());
          if (evt.has_prio())
            jevt->set_prio(evt.prio());
          if (evt.has_tag())
            jevt->set_tag(evt.tag());
          if (evt.has_message())
            jevt->set_message(evt.message());
          if (evt.has_comm())
            jevt->set_comm(evt.comm());
          if (evt.has_exe())
            jevt->set_exe(evt.exe());
          if (evt.has_systemd_unit())
            jevt->set_systemd_unit(evt.systemd_unit());
          if (evt.has_hostname())
            jevt->set_hostname(evt.hostname());
          if (evt.has_transport())
            jevt->set_transport(evt.transport());
          if (evt.has_monotonic_ts_us())
            jevt->set_monotonic_ts_us(evt.monotonic_ts_us());
        });
    module_context_->trace_packet_stream->Push(
        actual_ts, TracePacketData{std::move(tbv), state});
  }
  return ModuleResult::Handled();
}

void LinuxProbesModule::ParseTracePacketData(
    const protos::pbzero::TracePacket_Decoder& decoder,
    int64_t ts,
    const TracePacketData&,
    uint32_t field_id) {
  switch (field_id) {
    case TracePacket::kJournaldEventFieldNumber:
      parser_.ParseJournaldPacket(ts, decoder.journald_event());
      return;
    default:
      PERFETTO_FATAL("Unexpected field_id in LinuxProbesModule");
  }
}

}  // namespace perfetto::trace_processor
