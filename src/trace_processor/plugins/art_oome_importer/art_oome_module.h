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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_ART_OOME_IMPORTER_ART_OOME_MODULE_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_ART_OOME_IMPORTER_ART_OOME_MODULE_H_

#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"

namespace perfetto::trace_processor {

class DummyMemoryMapping;
class TraceProcessorContext;

class ArtOomeModule : public ProtoImporterModule {
 public:
  ArtOomeModule(ProtoImporterModuleContext* module_context,
                TraceProcessorContext* context);
  ~ArtOomeModule() override;

  void ParseTracePacketData(const protos::pbzero::TracePacket::Decoder& decoder,
                            int64_t ts,
                            const TracePacketData& data,
                            uint32_t field_id) override;

 private:
  void ParseArtProcessMetadata(int64_t ts, protozero::ConstBytes blob);

  TraceProcessorContext* const context_;
  DummyMemoryMapping* art_oome_mapping_ = nullptr;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_ART_OOME_IMPORTER_ART_OOME_MODULE_H_
