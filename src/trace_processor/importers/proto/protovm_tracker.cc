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

#include "src/trace_processor/importers/proto/protovm_tracker.h"

#include <memory>
#include <optional>

#include "perfetto/protozero/field.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/trace_processor/importers/common/import_logs_tracker.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

ProtoVmTracker::ProtoVmTracker(TraceProcessorContext* context)
    : context_(context) {}

ProtoVmTracker::~ProtoVmTracker() = default;

ProtoVmTracker* ProtoVmTracker::GetOrCreate(TraceProcessorContext* context) {
  if (!context->protovm_tracker) {
    context->protovm_tracker.reset(new ProtoVmTracker(context));
  }
  return static_cast<ProtoVmTracker*>(context->protovm_tracker.get());
}

void ProtoVmTracker::AddSequenceProducerMapping(uint32_t sequence_id,
                                                int32_t producer_id) {
  producer_id_to_sequence_ids_[producer_id].push_back(sequence_id);
}

void ProtoVmTracker::AddProtoVm(std::unique_ptr<protovm::Vm> vm,
                                const std::vector<int32_t>& producer_ids) {
  protovm::Vm* p = vm.get();
  vms_.push_back(std::move(vm));
  for (int32_t producer_id : producer_ids) {
    auto* sequence_ids = producer_id_to_sequence_ids_.Find(producer_id);
    if (!sequence_ids) {
      context_->storage->IncrementStats(stats::protovm_registration_error);
      continue;
    }
    for (auto sequence_id : *sequence_ids) {
      sequence_id_to_vms_[sequence_id].push_back(p);
    }
  }
}

std::optional<ProtoVmTracker::SerializedIncrementalState>
ProtoVmTracker::TryProcessPatch(
    const protos::pbzero::TracePacket::Decoder& decoder,
    const TraceBlobView& packet,
    RefPtr<PacketSequenceStateGeneration> sequence_state) {
  std::vector<protovm::Vm*>* vms =
      sequence_id_to_vms_.Find(decoder.trusted_packet_sequence_id());
  if (!vms) {
    return std::nullopt;
  }
  for (auto* vm : *vms) {
    auto status = vm->ApplyPatch({packet.data(), packet.size()});
    if (status.IsOk()) {
      return SerializeIncrementalState(*vm, decoder, packet,
                                       std::move(sequence_state));
    }
    if (status.IsAbort()) {
      context_->import_logs_tracker->RecordTokenizationError(
          stats::protovm_abort, packet.offset());
      return std::nullopt;
    }
  }
  return std::nullopt;
}

ProtoVmTracker::SerializedIncrementalState
ProtoVmTracker::SerializeIncrementalState(
    const protovm::Vm& vm,
    const protos::pbzero::TracePacket::Decoder& patch,
    const TraceBlobView& packet,
    RefPtr<PacketSequenceStateGeneration> sequence_state) const {
  protozero::HeapBuffered<protos::pbzero::TracePacket> proto;
  vm.SerializeIncrementalState(proto.get());
  proto->set_trusted_uid(patch.trusted_uid());
  proto->set_trusted_pid(patch.trusted_pid());
  proto->set_trusted_packet_sequence_id(patch.trusted_packet_sequence_id());
  auto [data, size] = proto.SerializeAsUniquePtr();
  auto blob = TraceBlob::TakeOwnership(std::move(data), size);

  uint64_t timestamp;
  protos::pbzero::TracePacket::Decoder state(
      protozero::ConstBytes{blob.data(), blob.size()});
  if (state.has_timestamp()) {
    timestamp = state.timestamp();
  } else {
    context_->import_logs_tracker->RecordTokenizationError(
        stats::protovm_incremental_state_without_timestamp, packet.offset());
    timestamp = patch.timestamp();
  }

  return SerializedIncrementalState{
      static_cast<int64_t>(timestamp),
      TracePacketData{TraceBlobView{std::move(blob)},
                      std::move(sequence_state)}};
}

}  // namespace perfetto::trace_processor
