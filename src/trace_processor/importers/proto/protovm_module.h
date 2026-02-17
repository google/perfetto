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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROTOVM_MODULE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROTOVM_MODULE_H_

#include "perfetto/trace_processor/trace_blob_view.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/importers/proto/protovm_tracker.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

class ProtoVmModule : public ProtoImporterModule {
 public:
  explicit ProtoVmModule(ProtoImporterModuleContext* context,
                         TraceProcessorContext* trace_context);
  ~ProtoVmModule() override;

  ModuleResult TokenizePacket(
      const protos::pbzero::TracePacket::Decoder& decoder,
      TraceBlobView* packet,
      int64_t packet_timestamp,
      RefPtr<PacketSequenceStateGeneration> state,
      uint32_t field_id) override;

 private:
  void ProcessTraceProvenancePacket(protozero::ConstBytes blob);
  void ProcessProtoVmsPacket(protozero::ConstBytes blob,
                             const TraceBlobView& packet);
  std::vector<int32_t> GetProducerIDs(
      const protos::pbzero::TracePacket::ProtoVms::Instance::Decoder& instance)
      const;

  TraceProcessorContext* trace_context_;
  ProtoVmTracker* protovm_tracker_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROTOVM_MODULE_H_
