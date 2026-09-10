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
#include "perfetto/protozero/message_handle.h"
#include "perfetto/protozero/root_message.h"
#include "perfetto/protozero/scattered_stream_writer.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/tracing/v2/shared_ring_buffer_writer.h"

namespace perfetto {
namespace protos {
namespace pbzero {
class TracePacket;
}  // namespace pbzero
}  // namespace protos

namespace tracing_v2 {

class SharedRingBuffer;

// TraceWriter implementation backed by a tracing v2 shared ring buffer.
//
// Packets use proto-group encoding because the reader may copy a fragment
// before nested-message lengths are known.
//
// A packet normally occupies one fragment. If it crosses a chunk boundary, the
// writer sets the continuation flags and carries on in the next chunk. The
// reader joins those fragments before rewriting the packet.
//
// As with TraceWriterImpl, an instance may be used by only one thread at a
// time. Packet writes go straight to the ring; the delegate handles reader
// notifications, flush completion and WriterID retirement.
class TraceWriterV2 : public TraceWriter,
                      public protozero::MessageFinalizationListener,
                      public protozero::ScatteredStreamWriter::Delegate {
 public:
  // Provides storage and coordinates reader progress for the writer's lifetime.
  // Calls run on SDK writer threads and may overlap across writers.
  // Post reader work to a shared sequence if it needs to run serially.
  class Delegate : public SharedRingBufferWriter::Delegate {
   public:
    ~Delegate() override;

    // Returns the same ring on every call. The ring and its backing memory must
    // remain valid for the delegate's lifetime.
    virtual SharedRingBuffer& ring_buffer() = 0;

    // Called after this writer publishes its data. For a non-empty callback:
    // - Wait for service acknowledgement before invoking it.
    // - The delegate chooses the callback sequence.
    // - Disconnect may discard it, as allowed by TraceWriter::Flush().
    virtual void Flush(WriterID, std::function<void()> callback) = 0;

    // Called after the writer publishes its last chunk. The WriterID must not
    // be reused while unconsumed ring positions can still name it.
    virtual void OnWriterDestroyed(WriterID) = 0;
  };

  struct InitArgs {
    // Must be non-null. Retained until the writer is destroyed.
    std::shared_ptr<Delegate> delegate;
    // Identifies this writer and the trace buffer its packets target.
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

  // Publishes this writer's data, then calls Delegate::Flush() (see above for
  // callback requirements).
  void Flush(std::function<void()> callback = {}) override;

  WriterID writer_id() const override {
    return ring_buffer_writer_.writer_id();
  }
  uint64_t written() const override { return stream_writer_.written(); }
  uint64_t drop_count() const override { return drop_count_; }

 private:
  // protozero::MessageFinalizationListener:
  void OnMessageFinalized(protozero::Message*) override;

  // protozero::ScatteredStreamWriter::Delegate:
  protozero::ContiguousMemoryRange GetNewBuffer() override;
  uint8_t* AnnotatePatch(uint8_t*) override;

  void ClosePacketFragment(bool continues_on_next);
  protozero::ContiguousMemoryRange EnterDropMode();

  // Outlives ring_buffer_writer_, which borrows its ring and delegate.
  const std::shared_ptr<Delegate> delegate_;
  SharedRingBufferWriter ring_buffer_writer_;

  protozero::ScatteredStreamWriter stream_writer_;

  // Kept behind a pointer to avoid including the generated TracePacket header.
  // The same root message is reset and reused for every packet.
  std::unique_ptr<protozero::RootMessage<protos::pbzero::TracePacket>>
      cur_packet_;

  // Start of the fragment currently being filled. Null while writes are going
  // to |drop_buffer_|.
  uint8_t* fragment_begin_ = nullptr;

  // Protozero writes here while the ring has no capacity. The bytes are thrown
  // away, but the data source can finish the packet normally.
  std::vector<uint8_t> drop_buffer_;

  bool packet_open_ = false;
  bool in_drop_mode_ = false;
  bool first_packet_on_sequence_ = true;

  uint64_t drop_count_ = 0;
};

}  // namespace tracing_v2
}  // namespace perfetto

#endif  // SRC_TRACING_V2_TRACE_WRITER_V2_H_
