/*
 * Copyright (C) 2021 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_METADATA_MODULE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_METADATA_MODULE_H_

#include "src/trace_processor/importers/common/trace_parser.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto {
namespace trace_processor {

class MetadataModule : public ProtoImporterModule {
 public:
  using ConstBytes = protozero::ConstBytes;
  explicit MetadataModule(TraceProcessorContext* context);

  ModuleResult TokenizePacket(
      const protos::pbzero::TracePacket::Decoder& decoder,
      TraceBlobView* packet,
      int64_t packet_timestamp,
      PacketSequenceState* state,
      uint32_t field_id) override;

  void ParseTracePacketData(const protos::pbzero::TracePacket::Decoder& decoder,
                            int64_t ts,
                            const TracePacketData&,
                            uint32_t field_id) override;

  void ParseTraceConfig(const protos::pbzero::TraceConfig_Decoder&) override;

 private:
  void ParseTrigger(int64_t ts, ConstBytes);
  void ParseTraceUuid(ConstBytes);

  TraceProcessorContext* context_;
  StringId producer_name_key_id_ = kNullStringId;
  StringId trusted_producer_uid_key_id_ = kNullStringId;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_METADATA_MODULE_H_
