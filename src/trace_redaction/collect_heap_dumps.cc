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

#include "src/trace_redaction/collect_heap_dumps.h"

#include "perfetto/base/compiler.h"
#include "perfetto/protozero/proto_decoder.h"
#include "src/trace_redaction/heap_dump_util.h"

#include "protos/third_party/android/art/heap_graph.pbzero.h"

namespace perfetto::trace_redaction {
namespace {

using TracePacket = protos::pbzero::TracePacket;
using HeapGraph = com::android::art::tracing::pbzero::HeapGraph;

}  // namespace

base::Status CollectHeapDumps::Collect(const TracePacket::Decoder& packet,
                                       Context* context) const {
  auto key = HeapDumpKey::Of(packet);
  if (PERFETTO_UNLIKELY(!key.has_value())) {
    return base::OkStatus();
  }

  bool has_heap_graph = packet.Get(kHeapGraphFieldNumber).valid();
  bool has_art_metadata = packet.has_art_process_metadata();
  if (!has_heap_graph && !has_art_metadata) {
    return base::OkStatus();
  }

  HeapDumpSequence& sequence = context->heap_dump_sequences[*key];

  if (has_heap_graph) {
    sequence.heap_graph_packets++;
    // `continued` is written late in HeapGraph and is absent when false, so
    // this is one walk over the message's top-level fields (objects are not
    // decoded). A dump is complete when its last heap_graph packet is not
    // continued.
    protozero::ProtoDecoder heap_graph(
        packet.Get(kHeapGraphFieldNumber).as_bytes());
    auto continued = heap_graph.FindField(HeapGraph::kContinuedFieldNumber);
    sequence.is_complete = !(continued.valid() && continued.as_bool());
  }

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
