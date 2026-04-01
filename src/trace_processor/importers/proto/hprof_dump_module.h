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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_HPROF_DUMP_MODULE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_HPROF_DUMP_MODULE_H_

#include <cstdint>
#include <memory>
#include <unordered_map>

#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/trace_processor/importers/art_hprof/art_hprof_parser.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"

namespace perfetto::trace_processor {

class TraceProcessorContext;

// Handles TracePacket.hprof_dump: feeds raw .hprof binary data to
// per-pid ArtHprofParser instances. Chunks are grouped by pid and
// finalized when last_chunk is set.
class HprofDumpModule : public ProtoImporterModule {
 public:
  HprofDumpModule(ProtoImporterModuleContext* module_context,
                  TraceProcessorContext* context);
  ~HprofDumpModule() override;

  void ParseTracePacketData(const protos::pbzero::TracePacket::Decoder& decoder,
                            int64_t ts,
                            const TracePacketData& data,
                            uint32_t field_id) override;

  void OnEventsFullyExtracted() override;

 private:
  art_hprof::ArtHprofParser* GetOrCreateParser(int32_t pid);
  void FinalizeParser(int32_t pid);

  TraceProcessorContext* context_;
  std::unordered_map<int32_t, std::unique_ptr<art_hprof::ArtHprofParser>>
      parsers_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_HPROF_DUMP_MODULE_H_
