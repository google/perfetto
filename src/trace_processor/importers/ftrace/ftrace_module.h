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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_FTRACE_FTRACE_MODULE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_FTRACE_FTRACE_MODULE_H_

#include "perfetto/base/build_config.h"
#include "src/trace_processor/importers/ftrace/ftrace_parser.h"
#include "src/trace_processor/importers/ftrace/ftrace_tokenizer.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/timestamped_trace_piece.h"
#include "src/trace_processor/trace_blob_view.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace trace_processor {

class FtraceModule
    : public ProtoImporterModuleBase<PERFETTO_BUILDFLAG(PERFETTO_TP_FTRACE)> {
 public:
  explicit FtraceModule(TraceProcessorContext* context)
      : ProtoImporterModuleBase(context),
        tokenizer_(context),
        parser_(context) {}

  ModuleResult TokenizePacket(
      const protos::pbzero::TracePacket::Decoder& decoder,
      TraceBlobView* packet,
      int64_t /*packet_timestamp*/,
      PacketSequenceState* /*state*/) {
    if (decoder.has_ftrace_events()) {
      auto ftrace_field = decoder.ftrace_events();
      const size_t fld_off = packet->offset_of(ftrace_field.data);
      tokenizer_.TokenizeFtraceBundle(
          packet->slice(fld_off, ftrace_field.size));
      return ModuleResult::Handled();
    }
    return ModuleResult::Ignored();
  }

  ModuleResult ParsePacket(const protos::pbzero::TracePacket::Decoder& decoder,
                           const TimestampedTracePiece&) {
    // TODO(eseckler): implement.
    if (decoder.has_ftrace_stats()) {
      parser_.ParseFtraceStats(decoder.ftrace_stats());
      return ModuleResult::Handled();
    }

    return ModuleResult::Ignored();
  }

  ModuleResult ParseFtracePacket(uint32_t cpu,
                                 const TimestampedTracePiece& ttp) {
    return parser_.ParseFtraceEvent(cpu, ttp);
  }

 private:
  FtraceTokenizer tokenizer_;
  FtraceParser parser_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_FTRACE_FTRACE_MODULE_H_
