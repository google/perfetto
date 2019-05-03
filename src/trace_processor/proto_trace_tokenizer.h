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

#ifndef SRC_TRACE_PROCESSOR_PROTO_TRACE_TOKENIZER_H_
#define SRC_TRACE_PROCESSOR_PROTO_TRACE_TOKENIZER_H_

#include <stdint.h>

#include <memory>
#include <vector>

#include "src/trace_processor/chunked_trace_reader.h"
#include "src/trace_processor/proto_incremental_state.h"
#include "src/trace_processor/trace_processor_impl.h"

#include "perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;
class TraceBlobView;
class TraceSorter;
class TraceStorage;

// Reads a protobuf trace in chunks and extracts boundaries of trace packets
// (or subfields, for the case of ftrace) with their timestamps.
class ProtoTraceTokenizer : public ChunkedTraceReader {
 public:
  // Scans the beginning of the trace for valid TracePackets to determine if the
  // trace contains TrackEvents.
  //
  // TODO(eseckler): This is a pretty bad hack to enable us to choose a
  // different sorting window size for traces with TrackEvents. We should
  // reconsider and redesign our sorting strategy, so that we don't need to
  // change global trace processor options if TrackEvents are present.
  static TraceType GuessProtoTraceType(const uint8_t* data, size_t size);

  // |reader| is the abstract method of getting chunks of size |chunk_size_b|
  // from a trace file with these chunks parsed into |trace|.
  explicit ProtoTraceTokenizer(TraceProcessorContext*);
  ~ProtoTraceTokenizer() override;

  // ChunkedTraceReader implementation.
  bool Parse(std::unique_ptr<uint8_t[]>, size_t size) override;

 private:
  void ParseInternal(std::unique_ptr<uint8_t[]> owned_buf,
                     uint8_t* data,
                     size_t size);
  void ParsePacket(TraceBlobView);
  void HandleIncrementalStateCleared(
      const protos::pbzero::TracePacket::Decoder& packet_decoder);
  void HandlePreviousPacketDropped(
      const protos::pbzero::TracePacket::Decoder& packet_decoder);
  void ParseInternedData(
      const protos::pbzero::TracePacket::Decoder& packet_decoder,
      TraceBlobView interned_data);
  void ParseThreadDescriptorPacket(
      const protos::pbzero::TracePacket::Decoder& packet_decoder);
  void ParseTrackEventPacket(
      const protos::pbzero::TracePacket::Decoder& packet_decoder,
      TraceBlobView packet);
  void ParseFtraceBundle(TraceBlobView);
  void ParseFtraceEvent(uint32_t cpu, TraceBlobView);

  ProtoIncrementalState::PacketSequenceState*
  GetIncrementalStateForPacketSequence(uint32_t sequence_id) {
    if (!incremental_state)
      incremental_state.reset(new ProtoIncrementalState());
    return incremental_state->GetOrCreateStateForPacketSequence(sequence_id);
  }

  TraceProcessorContext* context_;

  // Used to glue together trace packets that span across two (or more)
  // Parse() boundaries.
  std::vector<uint8_t> partial_buf_;

  // Temporary. Currently trace packets do not have a timestamp, so the
  // timestamp given is latest_timestamp_.
  int64_t latest_timestamp_ = 0;

  // Stores incremental state and references to interned data, e.g. for track
  // event protos.
  std::unique_ptr<ProtoIncrementalState> incremental_state;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_PROTO_TRACE_TOKENIZER_H_
