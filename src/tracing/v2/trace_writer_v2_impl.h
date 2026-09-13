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
// writer sets the continuation flags and carries on in the next chunk. The
// reader joins those fragments to reassemble the packet.
//
// A nested message can span chunks and still be open when the reader copies
// an earlier fragment. The two encodings handle this differently:
//
// - Length-delimited (v1): Protozero reserves a length field before each nested
//   message and fills it in when the message finishes. If the chunk containing
//   that field must be released first, TraceWriterImpl redirects the length
//   write to a patch record. Once the message finishes, the arbiter sends the
//   completed patch to the service to update its copy of the chunk.
//
// - Proto-group (this writer): Each nested message starts with a group tag and
//   ends with an appended closing byte. Previously published bytes need no
//   length update. After reassembly, ProtoRewriter converts the packet to
//   ordinary length-delimited protobuf.
//
// If the writer drops any part of a packet:
// - Its remaining bytes go to the drop buffer.
// - The next publication carries kFlagDataLoss. The reader discards all
//   published fragments in that chunk and reports the gap.
// - Packet reassembly discards orphan continuations. Later complete packets
//   in unflagged chunks can be delivered.
//
// As with TraceWriterImpl, an instance may be used by only one thread at a
// time. Packet writes go straight to the ring buffer. The delegate handles
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
  // Several writers can share this delegate. Each keeps it alive for as long
  // as it needs it, independently of when the other writers are destroyed.
  //
  // Refcounting manages lifetime only. Each writer still needs a single
  // calling thread or external synchronization.
  //
  // Calls run on the writer's thread and may overlap across writers.
  // Implementations must synchronize access to their shared state.
  class Delegate : public SharedRingBufferWriter::Delegate {
   public:
    ~Delegate() override;

    // Called after this writer publishes its data. For a non-empty callback:
    // - Wait for service acknowledgement before invoking it.
    // - The delegate chooses the callback sequence.
    // - Disconnect may discard it, as allowed by TraceWriter::Flush().
    virtual void Flush(WriterID, std::function<void()> callback) = 0;

    // The caller owns the writer, while the writer retains this delegate.
    // Releasing that reference does not tell the delegate which writer ended.
    // Called after this writer publishes its final chunk. The delegate decides
    // when the reader no longer needs the writer's resources and its WriterID
    // can be used by another writer.
    //
    // Like v1, where ~TraceWriterImpl() flushes remaining data and calls
    // SharedMemoryArbiterImpl::ReleaseWriterID():
    //
    //   Writer                               Delegate
    //   ~TraceWriterV2Impl()
    //     publish last chunk
    //     OnWriterDestroyed(id) -----------> arrange retirement of id
    //
    // Prototype note: InProcessTracingV2Bridge forwards v2 ring data through a
    // retained v1 writer. It waits for the reader to pass the final publication
    // before releasing that writer and its reassembly state, so unread packets
    // can still be forwarded. Once we have validated the v2 ring buffer and the
    // service reads it directly, check whether writer cleanup can be simpler
    // without the bridge.
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

  // Start of the fragment currently being filled. Null while writes are going
  // to |drop_buffer_|.
  uint8_t* fragment_begin_ = nullptr;

  // Protozero writes here while the ring buffer has no capacity. The bytes
  // are thrown away, but the data source can finish the packet normally.
  std::vector<uint8_t> drop_buffer_;

  bool packet_open_ = false;
  bool in_drop_mode_ = false;
  bool first_packet_on_sequence_ = true;

  uint64_t drop_count_ = 0;
};

}  // namespace tracing_v2
}  // namespace perfetto

#endif  // SRC_TRACING_V2_TRACE_WRITER_V2_IMPL_H_
