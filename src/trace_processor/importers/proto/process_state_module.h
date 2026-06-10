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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROCESS_STATE_MODULE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROCESS_STATE_MODULE_H_

#include <cstdint>

#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

// Decodes ProcessStateSnapshot trace packets emitted by the
// "android.process_state" data source (frameworks/base/services/.../am/
// ProcessStateTracer) into these SQL tables:
//   * android_process_state_snapshot         — one row per packet
//   * android_process_state_process          — one row per (snapshot, pid)
//   * android_process_state_uid              — one row per (snapshot, uid)
//   * android_process_state_service          — one row per (snapshot, service)
//   * android_process_state_binding          — one row per service binding
//   * android_process_state_provider         — one row per content provider
//   * android_process_state_provider_binding — one row per provider binding
class ProcessStateModule : public ProtoImporterModule {
 public:
  explicit ProcessStateModule(ProtoImporterModuleContext* module_context,
                              TraceProcessorContext* context);

  ~ProcessStateModule() override;

  void ParseTracePacketData(const protos::pbzero::TracePacket::Decoder& decoder,
                            int64_t ts,
                            const TracePacketData&,
                            uint32_t field_id) override;

 private:
  void ParseSnapshot(int64_t ts, protozero::ConstBytes blob);

  TraceProcessorContext* context_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROCESS_STATE_MODULE_H_
