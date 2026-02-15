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

#include "src/trace_processor/importers/proto/protovm_incremental_tracing.h"

#include <memory>

#include "perfetto/protozero/field.h"
#include "perfetto/protozero/scattered_heap_buffer.h"

#include "protos/perfetto/trace/perfetto/trace_provenance.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace trace_processor {

namespace {
TraceBlob MakeIncrementalStatePacket(
    const protovm::Vm& vm,
    const protos::pbzero::TracePacket::Decoder& patch) {
  std::string incremental_state_without_trusted_fields =
      vm.SerializeIncrementalState();

  protozero::HeapBuffered<protos::pbzero::TracePacket> incremental_state;
  incremental_state->AppendRawProtoBytes(
      incremental_state_without_trusted_fields.data(),
      incremental_state_without_trusted_fields.size());
  incremental_state->set_trusted_uid(patch.trusted_uid());
  incremental_state->set_trusted_pid(patch.trusted_pid());
  incremental_state->set_trusted_packet_sequence_id(
      patch.trusted_packet_sequence_id());

  auto serialized = incremental_state.SerializeAsString();
  return TraceBlob::CopyFrom(serialized.data(), serialized.size());
}

}  // namespace

void ProtoVmIncrementalTracing::ProcessTraceProvenancePacket(
    protozero::ConstBytes blob) {
  protos::pbzero::TraceProvenance::Decoder trace_provenance(blob);
  for (auto it_buf = trace_provenance.buffers(); it_buf; ++it_buf) {
    protos::pbzero::TraceProvenance::Buffer::Decoder buffer(*it_buf);
    for (auto it_seq = buffer.sequences(); it_seq; ++it_seq) {
      protos::pbzero::TraceProvenance::Sequence::Decoder sequence(*it_seq);
      sequence_id_to_producer_id_[sequence.id()] = sequence.producer_id();
    }
  }
}

void ProtoVmIncrementalTracing::InstantiateProtoVms(
    protozero::ConstBytes blob) {
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
      producer_id_to_vm_.emplace(*producer_id, vm);
    }
  }
}

protovm::StatusOr<TraceBlob> ProtoVmIncrementalTracing::TryProcessPatch(
    const TraceBlobView& blob) {
  protos::pbzero::TracePacket::Decoder patch(blob.data(), blob.size());
  if (!patch.has_trusted_packet_sequence_id()) {
    return protovm::StatusOr<TraceBlob>::Error();
  }
  auto producer_id =
      sequence_id_to_producer_id_.find(patch.trusted_packet_sequence_id());
  if (producer_id == sequence_id_to_producer_id_.cend()) {
    return protovm::StatusOr<TraceBlob>::Error();
  }
  auto [vm_begin, vm_end] = producer_id_to_vm_.equal_range(producer_id->second);
  for (auto vm = vm_begin; vm != vm_end; ++vm) {
    auto status = vm->second->ApplyPatch({blob.data(), blob.size()});
    if (status.IsOk()) {
      return MakeIncrementalStatePacket(*vm->second, patch);
    }
    if (status.IsAbort()) {
      return status;
    }
  }
  // Packet is not a valid patch (no ProtoVM instance could process it)
  return protovm::StatusOr<TraceBlob>::Error();
}

}  // namespace trace_processor
}  // namespace perfetto
