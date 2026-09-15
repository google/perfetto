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

#ifndef SRC_TRACING_V2_TRACE_WRITER_V2_IMPL_H_
#define SRC_TRACING_V2_TRACE_WRITER_V2_IMPL_H_

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
// A packet normally occupies one fragment. If it crosses a chunk boundary, the
// writer sets the continuation flags and continues in the next chunk. The
// reader joins those fragments to reassemble the packet.
//
// A nested message can span chunks and still be open when the reader copies
// an earlier fragment. The two encodings handle this differently:
//
// - Length-delimited (v1):
//   1. Protozero reserves a length field before each nested message.
//   2. If the chunk must be released before the message ends, TraceWriterImpl
//      redirects the length write to a patch record.
//   3. When the message ends, Protozero writes its length. The arbiter sends
//      any completed patch to the service, which updates its copy of the chunk.
//
// - Proto group (this writer):
//   1. Each nested message starts with a group tag.
//   2. When the message ends, Protozero appends a closing byte. Previously
//      published bytes need no length update.
//   3. After reassembly, ProtoRewriter converts the packet to ordinary
//      length-delimited protobuf.
//
// If the writer drops any part of a packet:
// - Its remaining bytes go to the drop buffer.
// - The next publication carries kFlagDataLoss. The reader discards all
//   published fragments in that chunk and reports the gap.
// - Packet reassembly discards orphan continuations. Later complete packets
//   in unflagged chunks can be delivered.
//
// As with TraceWriterImpl, use each instance from one thread at a time.
// Packet writes go directly to the ring buffer. The delegate handles
// reader notifications, flush completion and WriterID retirement.
class TraceWriterV2Impl : public TraceWriter,
                          public protozero::MessageFinalizationListener,
                          public protozero::ScatteredStreamWriter::Delegate {
 public:
  // Coordinates reader progress, flush completion and writer retirement.
  //
  // Independently owned writers can share a delegate:
  //
  //   Caller A                           Caller B
  //        | owns                             | owns
  //        v                                  v
  //   v2 writer A                        v2 writer B
  //        | shared_ptr                       | shared_ptr
  //        +----------------+-----------------+
  //                         v
  //                      Delegate
  //
  // Each writer retains its own reference until destruction. Destroying one
  // writer must not destroy a delegate that other writers still use.
  //
  // Refcounting manages lifetime only. Each writer still needs a single
  // calling thread or external synchronization.
  //
  // Calls run on each writer's thread and can overlap across writers.
  // Implementations must synchronize access to their shared state.
  class Delegate : public SharedRingBufferWriter::Delegate {
   public:
    ~Delegate() override;

    // Called after this writer publishes its data. For a non-empty callback:
    // - Invoke it after service acknowledgement.
    // - The delegate chooses the callback sequence.
    // - On disconnect, the delegate can discard it, as TraceWriter::Flush()
    //   permits.
    virtual void Flush(WriterID, std::function<void()> callback) = 0;

    // The caller owns the writer, while the writer retains this delegate.
    // Reference release alone does not identify which writer ended.
    // OnWriterDestroyed() supplies that identity after the final publication.
    //
    // The reader can still have unconsumed positions for this writer. The
    // delegate must retain its resources until the reader no longer needs them.
    // Only then can another writer use this WriterID.
    //
    // Like v1, where ~TraceWriterImpl() flushes remaining data and calls
    // SharedMemoryArbiterImpl::ReleaseWriterID():
    //
    //   Writer                               Delegate
    //   ~TraceWriterV2Impl()
    //     publish last chunk
    //     OnWriterDestroyed(id) -----------> arrange retirement of id
    //
    // In the prototype, InProcessTracingV2Bridge forwards v2 packets through a
    // retained v1 writer:
    // 1. The v2 writer publishes its final chunk and calls OnWriterDestroyed().
    // 2. The reader consumes the remaining positions. The bridge still needs
    //    the v1 writer and reassembly state to forward those packets.
    // 3. After the reader passes the final publication, the bridge can release
    //    the v1 writer and reassembly state.
    // Revisit this cleanup when the service reads the v2 ring buffer directly
    // and the bridge is no longer needed.
    virtual void OnWriterDestroyed(WriterID) = 0;
  };

  struct InitArgs {
    // Must be non-null. Each writer retains a reference until its destruction.
    std::shared_ptr<Delegate> delegate;
    // Identifies this writer and the trace buffer its packets target.
    WriterID writer_id = 0;
    BufferID target_buffer = 0;
    BufferExhaustedPolicy buffer_exhausted_policy =
        BufferExhaustedPolicy::kDrop;
    // Must be non-null. Borrowed by the writer. The ring buffer and its backing
    // memory must outlive the writer.
    SharedRingBuffer* ring_buffer = nullptr;
  };

  explicit TraceWriterV2Impl(const InitArgs&);
  ~TraceWriterV2Impl() override;

  TraceWriterV2Impl(const TraceWriterV2Impl&) = delete;
  TraceWriterV2Impl& operator=(const TraceWriterV2Impl&) = delete;
  TraceWriterV2Impl(TraceWriterV2Impl&&) = delete;
  TraceWriterV2Impl& operator=(TraceWriterV2Impl&&) = delete;

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

  // Outlives ring_buffer_writer_, which borrows the delegate.
  const std::shared_ptr<Delegate> delegate_;
  SharedRingBufferWriter ring_buffer_writer_;

  protozero::ScatteredStreamWriter stream_writer_;

  // Kept behind a pointer to avoid including the generated TracePacket header.
  // The same root message is reset and reused for every packet.
  std::unique_ptr<protozero::RootMessage<protos::pbzero::TracePacket>>
      cur_packet_;

  // Start of the open fragment in shared memory. Null after the fragment closes
  // and while writes use |drop_buffer_|.
  uint8_t* fragment_begin_ = nullptr;

  // If a packet is dropped, Protozero writes its remaining bytes here so the
  // data source can finish the packet. These bytes are never published.
  // Each writer needs its own buffer because writes use ordinary stores.
  // A global buffer would allow concurrent writers to modify the same bytes.
  //
  // Allocate this buffer only when needed, then reuse it for later drops.
  // Writers that never drop avoid the extra memory. The tradeoff is an
  // allocation on the first drop. Match the chunk size so raw stream callers
  // can still reserve a contiguous range as large as a normal fragment.
  std::vector<uint8_t> drop_buffer_;

  bool packet_open_ = false;
  bool in_drop_mode_ = false;
  bool first_packet_on_sequence_ = true;

  uint64_t drop_count_ = 0;
};

}  // namespace tracing_v2
}  // namespace perfetto

#endif  // SRC_TRACING_V2_TRACE_WRITER_V2_IMPL_H_
