/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_SYSTEM_PROBES_MODULE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_SYSTEM_PROBES_MODULE_H_

#include "perfetto/base/build_config.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/importers/proto/system_probes_parser.h"
#include "src/trace_processor/timestamped_trace_piece.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace trace_processor {

class SystemProbesModule : public ProtoImporterModuleBase<PERFETTO_BUILDFLAG(
                               PERFETTO_TP_SYSTEM_PROBES)> {
 public:
  explicit SystemProbesModule(TraceProcessorContext* context)
      : ProtoImporterModuleBase(context), parser_(context) {}

  ModuleResult ParsePacket(const protos::pbzero::TracePacket::Decoder& decoder,
                           const TimestampedTracePiece& ttp) {
    if (decoder.has_process_tree()) {
      parser_.ParseProcessTree(decoder.process_tree());
      return ModuleResult::Handled();
    }

    if (decoder.has_process_stats()) {
      parser_.ParseProcessStats(ttp.timestamp, decoder.process_stats());
      return ModuleResult::Handled();
    }

    if (decoder.has_sys_stats()) {
      parser_.ParseSysStats(ttp.timestamp, decoder.sys_stats());
      return ModuleResult::Handled();
    }

    if (decoder.has_system_info()) {
      parser_.ParseSystemInfo(decoder.system_info());
      return ModuleResult::Handled();
    }

    return ModuleResult::Ignored();
  }

 private:
  SystemProbesParser parser_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_SYSTEM_PROBES_MODULE_H_
