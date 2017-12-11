/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_TRACING_CORE_TRACE_PACKET_H_
#define INCLUDE_PERFETTO_TRACING_CORE_TRACE_PACKET_H_

#include <stddef.h>

#include <memory>

#include "perfetto/base/logging.h"
#include "perfetto/tracing/core/chunk.h"

class TracePacket;

namespace perfetto {

namespace protos {
class TracePacket;  // From protos/trace_packet.pb.h.
}  // namespace protos

// A wrapper around a byte buffer that contains a protobuf-encoded TracePacket
// (see trace_packet.proto). The TracePacket is decoded only if the Consumer
// requests that. This is to allow Consumer(s) to just stream the packet over
// the network or save it to a file without wasting time decoding it and without
// needing to depend on libprotobuf or the trace_packet.pb.h header.
// If the packets are saved / streamed and not just consumed locally, consumers
// should ensure to preserve the unknown fields in the proto. A consumer, in
// fact, might have an older version .proto which is newer on the producer.
class TracePacket {
 public:
  using const_iterator = ChunkSequence::const_iterator;

  using DecodedTracePacket = protos::TracePacket;
  TracePacket();
  ~TracePacket();
  TracePacket(TracePacket&&) noexcept;
  TracePacket& operator=(TracePacket&&);

  // Accesses all the raw chunks in the packet, for saving them to file/network.
  const_iterator begin() const { return chunks_.begin(); }
  const_iterator end() const { return chunks_.end(); }

  // Decodes the packet for inline use.
  bool Decode();

  // Must explicitly call Decode() first.
  const DecodedTracePacket* operator->() {
    PERFETTO_DCHECK(decoded_packet_);
    return decoded_packet_.get();
  }
  const DecodedTracePacket& operator*() { return *(operator->()); }

  // Mutator, used only by the service and tests.
  void AddChunk(Chunk);

 private:
  TracePacket(const TracePacket&) = delete;
  TracePacket& operator=(const TracePacket&) = delete;

  // TODO(primiano): who owns the memory of the chunks? Figure out later.

  ChunkSequence chunks_;  // Not owned.
  std::unique_ptr<DecodedTracePacket> decoded_packet_;
};

}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_TRACING_CORE_TRACE_PACKET_H_
