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

#ifndef SRC_TRACING_V2_TRACE_WRITER_V2_H_
#define SRC_TRACING_V2_TRACE_WRITER_V2_H_

#include <stdint.h>

#include <functional>
#include <memory>
#include <vector>

#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/protozero/contiguous_memory_range.h"
#include "perfetto/protozero/root_message.h"
#include "perfetto/protozero/scattered_stream_writer.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/tracing/v2/ring_writer.h"

namespace perfetto {
namespace protos {
namespace pbzero {
class TracePacket;
}  // namespace pbzero
}  // namespace protos

namespace tracing_v2 {

class SharedRingBuffer;

// The SDK-facing writer for the tracing v2 ring: a TraceWriter that encodes
// packets with the private start-tag-and-terminator framing and appends them to
// the ring as fragments, so that nothing ever has to be written behind bytes a
// reader may already have taken.
//
// One packet occupies one fragment, except where it runs out of room in its
// chunk: then it is split, the chunk it filled is published with "continues on
// next chunk" and the next one carries "continues from previous chunk". The
// relay reassembles the pieces and rewrites the private framing to ordinary
// length-delimited protobuf before the packet reaches the trace buffer.
//
// Threading: like a v1 TraceWriter, one instance belongs to one thread at a
// time. The writer touches the ring directly on the hot path; the delegate is
// for lifetime and notification, never for per-packet work.
class TraceWriterV2 : public TraceWriter,
                      public protozero::ScatteredStreamWriter::Delegate {
 public:
  // The only thing this writer needs from whoever owns the ring. It is held by
  // shared_ptr so the ring cannot go away underneath a writer the SDK still
  // holds in thread-local storage.
  //
  // Deliberately not a v1-sized arbiter interface: there is no flush watermark,
  // no retirement position and no per-writer acknowledgement here. Step 1 does
  // not have a durable-commit protocol to hang them on.
  class Delegate {
   public:
    virtual ~Delegate();
    // Something is in the ring. Coalescing this into one drain pass is the
    // implementation's business; this writer calls it whenever it may have
    // moved write_pos, including for a reservation that produced no payload.
    virtual void OnPacketsCommitted() = 0;
  };

  struct InitArgs {
    SharedRingBuffer* ring_buffer = nullptr;
    std::shared_ptr<Delegate> delegate;
    // Borrowed from the downstream v1 writer for Step 1. A direct v2 producer
    // will get its identity and its routing from service negotiation instead.
    WriterID writer_id = 0;
    BufferID target_buffer = 0;
    BufferExhaustedPolicy buffer_exhausted_policy =
        BufferExhaustedPolicy::kDrop;
  };

  explicit TraceWriterV2(const InitArgs&);
  ~TraceWriterV2() override;

  TraceWriterV2(const TraceWriterV2&) = delete;
  TraceWriterV2& operator=(const TraceWriterV2&) = delete;
  TraceWriterV2(TraceWriterV2&&) = delete;
  TraceWriterV2& operator=(TraceWriterV2&&) = delete;

  // TraceWriter:
  TracePacketHandle NewTracePacket() override;
  void FinishTracePacket() override;
  // Finishes any open packet and nudges the reader, then runs |callback|
  // inline. It is NOT proof that the bytes reached the service or the final
  // trace buffer: Step 1 has no acknowledgement path back from the relay, so
  // there is nothing to wait for. Callers that need durability need the
  // producer-to-service protocol that replaces this scaffolding.
  void Flush(std::function<void()> callback = {}) override;
  WriterID writer_id() const override;
  uint64_t written() const override;
  uint64_t drop_count() const override;

 private:
  // protozero::ScatteredStreamWriter::Delegate:
  protozero::ContiguousMemoryRange GetNewBuffer() override;
  uint8_t* AnnotatePatch(uint8_t*) override;

  void ClosePacketFragment(bool continues_on_next);
  protozero::ContiguousMemoryRange EnterDropMode();

  RingWriter ring_writer_;
  const std::shared_ptr<Delegate> delegate_;

  protozero::ScatteredStreamWriter stream_writer_;
  // Held by pointer so the generated TracePacket header stays out of this
  // one; it is created once in the constructor and reused for every packet.
  std::unique_ptr<protozero::RootMessage<protos::pbzero::TracePacket>>
      current_packet_;

  // Start of the fragment the encoder is currently filling, so the number of
  // bytes it used is write_ptr() - fragment_begin_. Null when no fragment is
  // open, which is also how a dropped packet is recognised.
  uint8_t* fragment_begin_ = nullptr;

  // Where the encoder is sent while this writer has no ring capacity. Its
  // contents are overwritten and thrown away; it exists so that a data source
  // in the middle of a packet does not have to care.
  std::vector<uint8_t> drop_buffer_;

  bool packet_open_ = false;
  uint64_t written_ = 0;
  uint64_t drop_count_ = 0;
  bool dropping_ = false;
};

}  // namespace tracing_v2
}  // namespace perfetto

#endif  // SRC_TRACING_V2_TRACE_WRITER_V2_H_
