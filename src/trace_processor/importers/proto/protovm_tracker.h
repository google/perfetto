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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROTOVM_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROTOVM_TRACKER_H_

#include <memory>
#include <optional>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/protovm/vm.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/types/destructible.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_processor {

class TraceProcessorContext;

class ProtoVmTracker : public Destructible {
 public:
  struct SerializedIncrementalState {
    int64_t timestamp;
    TracePacketData data;
  };

  explicit ProtoVmTracker(TraceProcessorContext* context);
  ~ProtoVmTracker() override;

  static ProtoVmTracker* GetOrCreate(TraceProcessorContext* context);

  void AddSequenceProducerMapping(uint32_t sequence_id, int32_t producer_id);

  void AddProtoVm(std::unique_ptr<protovm::Vm> vm,
                  const std::vector<int32_t>& producer_ids);

  std::optional<SerializedIncrementalState> TryProcessPatch(
      const protos::pbzero::TracePacket::Decoder& decoder,
      const TraceBlobView& packet,
      RefPtr<PacketSequenceStateGeneration> sequence_state);

 private:
  SerializedIncrementalState SerializeIncrementalState(
      const protovm::Vm& vm,
      const protos::pbzero::TracePacket::Decoder& patch,
      const TraceBlobView& packet,
      RefPtr<PacketSequenceStateGeneration> sequence_state) const;

  TraceProcessorContext* const context_;
  base::FlatHashMap<int32_t, std::vector<uint32_t>>
      producer_id_to_sequence_ids_;
  base::FlatHashMap<uint32_t, std::vector<protovm::Vm*>> sequence_id_to_vms_;
  std::vector<std::unique_ptr<protovm::Vm>> vms_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROTOVM_TRACKER_H_
