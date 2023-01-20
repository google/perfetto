/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROTO_TRACE_READER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROTO_TRACE_READER_H_

#include <stdint.h>

#include <memory>

#include "src/trace_processor/importers/common/chunked_trace_reader.h"
#include "src/trace_processor/importers/proto/proto_incremental_state.h"
#include "src/trace_processor/importers/proto/proto_trace_tokenizer.h"

namespace protozero {
struct ConstBytes;
}

namespace perfetto {

namespace protos {
namespace pbzero {
class TracePacket_Decoder;
class TraceConfig_Decoder;
}  // namespace pbzero
}  // namespace protos

namespace trace_processor {

class PacketSequenceState;
class TraceProcessorContext;
class TraceSorter;
class TraceStorage;

// Implementation of ChunkedTraceReader for proto traces. Tokenizes a proto
// trace into packets, handles parsing of any packets which need to be
// handled in trace-order and passes the remainder to TraceSorter to sort
// into timestamp order.
class ProtoTraceReader : public ChunkedTraceReader {
 public:
  // |reader| is the abstract method of getting chunks of size |chunk_size_b|
  // from a trace file with these chunks parsed into |trace|.
  explicit ProtoTraceReader(TraceProcessorContext*);
  ~ProtoTraceReader() override;

  // ChunkedTraceReader implementation.
  util::Status Parse(TraceBlobView) override;
  void NotifyEndOfFile() override;

 private:
  using ConstBytes = protozero::ConstBytes;
  util::Status ParsePacket(TraceBlobView);
  util::Status ParseServiceEvent(int64_t ts, ConstBytes);
  util::Status ParseClockSnapshot(ConstBytes blob, uint32_t seq_id);
  void HandleIncrementalStateCleared(
      const protos::pbzero::TracePacket_Decoder&);
  void HandleFirstPacketOnSequence(uint32_t packet_sequence_id);
  void HandlePreviousPacketDropped(const protos::pbzero::TracePacket_Decoder&);
  void ParseTracePacketDefaults(const protos::pbzero::TracePacket_Decoder&,
                                TraceBlobView trace_packet_defaults);
  void ParseInternedData(const protos::pbzero::TracePacket_Decoder&,
                         TraceBlobView interned_data);
  void ParseTraceConfig(ConstBytes);

  base::Optional<StringId> GetBuiltinClockNameOrNull(uint64_t clock_id);

  PacketSequenceState* GetIncrementalStateForPacketSequence(
      uint32_t sequence_id) {
    if (!incremental_state)
      incremental_state.reset(new ProtoIncrementalState(context_));
    return incremental_state->GetOrCreateStateForPacketSequence(sequence_id);
  }
  util::Status ParseExtensionDescriptor(ConstBytes descriptor);

  TraceProcessorContext* context_;

  ProtoTraceTokenizer tokenizer_;

  // Temporary. Currently trace packets do not have a timestamp, so the
  // timestamp given is latest_timestamp_.
  int64_t latest_timestamp_ = 0;

  // Stores incremental state and references to interned data, e.g. for track
  // event protos.
  std::unique_ptr<ProtoIncrementalState> incremental_state;

  StringId skipped_packet_key_id_;
  StringId invalid_incremental_state_key_id_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROTO_TRACE_READER_H_
