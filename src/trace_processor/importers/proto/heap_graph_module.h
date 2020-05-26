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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_HEAP_GRAPH_MODULE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_HEAP_GRAPH_MODULE_H_

#include "perfetto/base/build_config.h"
#include "src/trace_processor/importers/proto/heap_graph_tracker.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/timestamped_trace_piece.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace trace_processor {

class HeapGraphModule : public ProtoImporterModuleBase<PERFETTO_BUILDFLAG(
                            PERFETTO_TP_HEAP_GRAPHS)> {
 public:
  explicit HeapGraphModule(TraceProcessorContext* context)
      : ProtoImporterModuleBase(context) {
    context_->heap_graph_tracker.reset(new HeapGraphTracker(context_));
  }

  ModuleResult ParsePacket(const protos::pbzero::TracePacket::Decoder& decoder,
                           const TimestampedTracePiece& ttp) {
    if (decoder.has_heap_graph()) {
      ParseHeapGraph(ttp.timestamp, decoder.heap_graph());
      return ModuleResult::Handled();
    }

    if (decoder.has_deobfuscation_mapping()) {
      ParseDeobfuscationMapping(decoder.deobfuscation_mapping());
      return ModuleResult::Handled();
    }
    return ModuleResult::Ignored();
  }

 private:
  void ParseHeapGraph(int64_t ts, protozero::ConstBytes);
  void ParseDeobfuscationMapping(protozero::ConstBytes);
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_HEAP_GRAPH_MODULE_H_
