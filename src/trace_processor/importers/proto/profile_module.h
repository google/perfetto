/*
 * Copyright (C) 2020 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROFILE_MODULE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROFILE_MODULE_H_

#include <cstdint>
#include <map>
#include "perfetto/protozero/field.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/importers/proto/perf_sample_tracker.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/storage/trace_storage.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_processor {

// Importer module for heap and CPU sampling profile data.
// TODO(eseckler): consider moving heap profiles here as well.
class ProfileModule : public ProtoImporterModule {
 public:
  explicit ProfileModule(ProtoImporterModuleContext* module_context,
                         TraceProcessorContext* context);
  ~ProfileModule() override;

  ModuleResult TokenizePacket(
      const protos::pbzero::TracePacket::Decoder& decoder,
      TraceBlobView* packet,
      int64_t packet_timestamp,
      RefPtr<PacketSequenceStateGeneration> state,
      uint32_t field_id) override;

  void ParseTracePacketData(const protos::pbzero::TracePacket::Decoder& decoder,
                            int64_t ts,
                            const TracePacketData& data,
                            uint32_t field_id) override;

  void NotifyEndOfFile() override;

 private:
  // chrome stack sampling:
  ModuleResult TokenizeStreamingProfilePacket(
      RefPtr<PacketSequenceStateGeneration>,
      TraceBlobView* packet,
      protozero::ConstBytes streaming_profile_packet);
  void ParseStreamingProfilePacket(
      int64_t timestamp,
      PacketSequenceStateGeneration*,
      protozero::ConstBytes streaming_profile_packet);

  // perf event profiling:
  void ParsePerfSample(int64_t ts,
                       PacketSequenceStateGeneration* sequence_state,
                       const protos::pbzero::TracePacket::Decoder& decoder);

  // heap profiling:
  void ParseProfilePacket(int64_t ts,
                          PacketSequenceStateGeneration*,
                          protozero::ConstBytes,
                          const protos::pbzero::TracePacket::Decoder& decoder);
  void ParseStreamingAllocation(
      int64_t ts,
      PacketSequenceStateGeneration* sequence_state,
      const protos::pbzero::TracePacket::Decoder& decoder);
  void ParseModuleSymbols(protozero::ConstBytes);
  void ParseSmapsPacket(int64_t ts, protozero::ConstBytes);

  TraceProcessorContext* context_;
  PerfSampleTracker perf_sample_tracker_;
  struct PendingStreamingAlloc {
    int64_t timestamp;
    uint64_t address;
    uint64_t size;
    uint64_t sample_size;
    uint32_t heap_id;
    UniquePid upid;
  };
  std::map<uint64_t, PendingStreamingAlloc> pending_streaming_allocs_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROFILE_MODULE_H_
