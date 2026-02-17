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

#include "src/trace_processor/importers/proto/protovm_module.h"

#include <memory>

#include "perfetto/protozero/field.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "protos/perfetto/trace/perfetto/trace_provenance.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/trace_processor/importers/common/import_logs_tracker.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

ProtoVmModule::ProtoVmModule(ProtoImporterModuleContext* context,
                             TraceProcessorContext* trace_context)
    : ProtoImporterModule(context),
      trace_context_(trace_context),
      protovm_tracker_(ProtoVmTracker::GetOrCreate(trace_context_)) {
  RegisterForField(protos::pbzero::TracePacket::kTraceProvenanceFieldNumber);
  RegisterForField(protos::pbzero::TracePacket::kProtovmsFieldNumber);
}

ProtoVmModule::~ProtoVmModule() = default;

ModuleResult ProtoVmModule::TokenizePacket(
    const protos::pbzero::TracePacket::Decoder& decoder,
    TraceBlobView* packet,
    int64_t /* packet_timestamp */,
    RefPtr<PacketSequenceStateGeneration> /* state */,
    uint32_t field_id) {
  if (field_id == protos::pbzero::TracePacket::kTraceProvenanceFieldNumber) {
    ProcessTraceProvenancePacket(decoder.trace_provenance());
    return ModuleResult::Ignored();
  }
  if (field_id == protos::pbzero::TracePacket::kProtovmsFieldNumber) {
    ProcessProtoVmsPacket(decoder.protovms(), *packet);
    return ModuleResult::Ignored();
  }
  return ModuleResult::Ignored();
}

void ProtoVmModule::ProcessTraceProvenancePacket(protozero::ConstBytes blob) {
  protos::pbzero::TraceProvenance::Decoder trace_provenance(blob);
  for (auto it_buf = trace_provenance.buffers(); it_buf; ++it_buf) {
    protos::pbzero::TraceProvenance::Buffer::Decoder buffer(*it_buf);
    for (auto it_seq = buffer.sequences(); it_seq; ++it_seq) {
      protos::pbzero::TraceProvenance::Sequence::Decoder sequence(*it_seq);
      protovm_tracker_->AddSequenceProducerMapping(
          sequence.id(), static_cast<int32_t>(sequence.producer_id()));
    }
  }
}

void ProtoVmModule::ProcessProtoVmsPacket(protozero::ConstBytes blob,
                                          const TraceBlobView& /*packet*/) {
  protos::pbzero::TracePacket::ProtoVms::Decoder decoder(blob);
  for (auto it = decoder.instance(); it; ++it) {
    protos::pbzero::TracePacket::ProtoVms::Instance::Decoder instance(*it);
    protozero::ConstBytes state = instance.has_state()
                                      ? instance.state()
                                      : protozero::ConstBytes{nullptr, 0};
    auto vm = std::make_unique<protovm::Vm>(
        instance.program(), 1024 * instance.memory_limit_kb(), state);
    protovm_tracker_->AddProtoVm(std::move(vm), GetProducerIDs(instance));
  }
}

std::vector<int32_t> ProtoVmModule::GetProducerIDs(
    const protos::pbzero::TracePacket::ProtoVms::Instance::Decoder& instance)
    const {
  std::vector<int32_t> ids;
  for (auto id = instance.producer_id(); id; ++id) {
    ids.push_back(*id);
  }
  return ids;
}

}  // namespace trace_processor
}  // namespace perfetto
