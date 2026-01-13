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
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "protos/perfetto/trace/perfetto/trace_provenance.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/trace_processor/importers/common/import_logs_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

ProtoVmModule::ProtoVmModule(ProtoImporterModuleContext* context,
                             TraceProcessorContext* trace_context)
    : ProtoImporterModule(context), trace_context_(trace_context) {
  RegisterForField(protos::pbzero::TracePacket::kTraceProvenanceFieldNumber);
  RegisterForField(protos::pbzero::TracePacket::kProtovmsFieldNumber);
  RegisterForField(
      protos::pbzero::TracePacket::kSurfaceflingerTransactionsFieldNumber);
}

ProtoVmModule::~ProtoVmModule() = default;

ModuleResult ProtoVmModule::TokenizePacket(
    const protos::pbzero::TracePacket::Decoder& decoder,
    TraceBlobView* packet,
    int64_t packet_timestamp,
    RefPtr<PacketSequenceStateGeneration> state,
    uint32_t field_id) {
  if (field_id == protos::pbzero::TracePacket::kTraceProvenanceFieldNumber) {
    ProcessTraceProvenancePacket(decoder.trace_provenance());
    return ModuleResult::Ignored();
  }
  if (field_id == protos::pbzero::TracePacket::kProtovmsFieldNumber) {
    ProcessProtoVmsPacket(decoder.protovms());
    return ModuleResult::Ignored();
  }
  return TryProcessPatch(decoder, packet, packet_timestamp, state);
}

void ProtoVmModule::ProcessTraceProvenancePacket(protozero::ConstBytes blob) {
  protos::pbzero::TraceProvenance::Decoder trace_provenance(blob);
  for (auto it_buf = trace_provenance.buffers(); it_buf; ++it_buf) {
    protos::pbzero::TraceProvenance::Buffer::Decoder buffer(*it_buf);
    for (auto it_seq = buffer.sequences(); it_seq; ++it_seq) {
      protos::pbzero::TraceProvenance::Sequence::Decoder sequence(*it_seq);
      producer_id_to_sequence_ids_[sequence.producer_id()].push_back(
          sequence.id());
    }
  }
}

void ProtoVmModule::ProcessProtoVmsPacket(protozero::ConstBytes blob) {
  protos::pbzero::TracePacket::ProtoVms::Decoder decoder(blob);
  for (auto it = decoder.instance(); it; ++it) {
    protos::pbzero::TracePacket::ProtoVms::Instance::Decoder instance(*it);
    protozero::ConstBytes state = instance.has_state()
                                      ? instance.state()
                                      : protozero::ConstBytes{nullptr, 0};
    vms_.push_back(std::make_unique<protovm::Vm>(
        instance.program(), 1024 * instance.memory_limit_kb(), state));
    protovm::Vm* vm = vms_.back().get();
    for (auto producer_id = instance.producer_id(); producer_id;
         ++producer_id) {
      auto* sequence_ids = producer_id_to_sequence_ids_.Find(*producer_id);
      PERFETTO_CHECK(sequence_ids);  // TODO: increment stats
      for (auto sequence_id : *sequence_ids) {
        sequence_id_to_vms_[sequence_id].push_back(vm);
      }
    }
  }
  producer_id_to_sequence_ids_.Clear();
}

ModuleResult ProtoVmModule::TryProcessPatch(
    const protos::pbzero::TracePacket::Decoder& decoder,
    TraceBlobView* packet,
    int64_t,
    RefPtr<PacketSequenceStateGeneration> state) {
  std::vector<protovm::Vm*>* vms =
      sequence_id_to_vms_.Find(decoder.trusted_packet_sequence_id());
  if (!vms) {
    return ModuleResult::Ignored();
  }
  for (auto* vm : *vms) {
    auto status = vm->ApplyPatch({packet->data(), packet->size()});
    if (status.IsOk()) {
      protozero::HeapBuffered<protozero::Message> incremental_state;
      TraceBlob serialized = SerializeIncrementalState(*vm, decoder);
      // TODO(lalitm): I suspect there will be discussions about this. FYI here
      // we read the timestamp from the serialized incremental state, so that a
      // ProtoVM's program also has the power of setting timestamps. Primiano
      // wanted this feature so you might want to chat directly with him. We can
      // always make the implementation below more efficient avoiding to
      // construct a full TracePacket::Decoder.
      protos::pbzero::TracePacket::Decoder serialized_decoder(
          protozero::ConstBytes{serialized.data(), serialized.size()});
      auto serialized_timestamp = serialized_decoder.timestamp();
      module_context_->trace_packet_stream->Push(
          static_cast<int64_t>(serialized_timestamp),
          TracePacketData{TraceBlobView{std::move(serialized)},
                          std::move(state)});
      return ModuleResult::Handled();
    }
    if (status.IsAbort()) {
      trace_context_->import_logs_tracker->RecordTokenizationError(
          stats::protovm_abort, packet->offset());
      return ModuleResult::Handled();
    }
  }
  return ModuleResult::Ignored();
}

TraceBlob ProtoVmModule::SerializeIncrementalState(
    const protovm::Vm& vm,
    const protos::pbzero::TracePacket::Decoder& patch) const {
  protozero::HeapBuffered<protos::pbzero::TracePacket> proto;
  vm.SerializeIncrementalState(proto.get());
  proto->set_trusted_uid(patch.trusted_uid());
  proto->set_trusted_pid(patch.trusted_pid());
  proto->set_trusted_packet_sequence_id(patch.trusted_packet_sequence_id());
  auto [data, size] = proto.SerializeAsUniquePtr();
  return TraceBlob::TakeOwnership(std::move(data), size);
}

}  // namespace trace_processor
}  // namespace perfetto
