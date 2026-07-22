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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_V8_CPU_PROFILE_MODULE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_V8_CPU_PROFILE_MODULE_H_

#include <cstdint>
#include <optional>
#include "perfetto/protozero/field.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/v8_tables_py.h"

namespace perfetto::trace_processor {

class TraceProcessorContext;
class PacketSequenceStateGeneration;

// Module that handles V8 CPU profile session packets and exposes helpers to
// parse the V8-specific extension fields that ride along the generic
// Frame / StreamingProfilePacket messages emitted by V8 CPU profiler.
class V8CpuProfileModule : public ProtoImporterModule {
 public:
  V8CpuProfileModule(ProtoImporterModuleContext* module_context,
                     TraceProcessorContext* context);
  ~V8CpuProfileModule() override;

  ModuleResult TokenizePacket(const TokenizePacketArgs& args) override;

  void ParseField(const ParseFieldArgs& args) override;

  static void OnFrameInterned(TraceProcessorContext* context,
                              PacketSequenceStateGeneration* state,
                              FrameId frame_id,
                              const uint8_t* frame_bytes,
                              size_t frame_size);

 private:
  void ParseTracePacketData(protozero::ConstBytes bytes,
                            int64_t ts,
                            uint32_t sequence_id);

  TraceProcessorContext* const context_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_V8_CPU_PROFILE_MODULE_H_
