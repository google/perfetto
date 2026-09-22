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

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "perfetto/base/proc_utils.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/protozero/message_handle.h"
#include "perfetto/protozero/root_message.h"
#include "perfetto/protozero/scattered_stream_writer.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/tracing/v2/shared_ring_buffer_writer.h"

namespace perfetto::tracing_v2 {

class SharedRingBuffer;
class ProducerRingBufferEndpoint;

// TraceWriter implementation backed by a tracing v2 shared ring buffer.
//
// Data path from the SDK to the service:
//
//          v1: SMB                          v2: ring buffer
//
//   data source (SDK)                 data source (SDK)
//     | NewTracePacket()                | NewTracePacket()
//     v                                 v
//   TraceWriterImpl                   TraceWriterV2Impl (this class)
//     |                                 |
//     | GetNewChunk()                   | BeginFragment(), EndFragment()
//     v                                 v
//   SharedMemoryArbiterImpl           SharedRingBufferWriter
//     | picks a free chunk,             | claims and publishes chunks
//     | batches completed chunks        | lock-free. The bytes do not
//     |                                 | go through
//     |                                 | ProducerRingBufferEndpoint.
//     v                                 v
//   [ SMB ]                           [ ring buffer ]
//     ^                                 ^
//     | copies the listed chunks        | copies all published chunks
//     |                                 |
//   service <-- CommitData IPC        service <-- DrainRingBuffer IPC
//               from the arbiter:                 from
//                                                 ProducerRingBufferEndpoint:
//               "chunks N, M are                  "read what is
//               complete"                         published"
//
// A nested message can span chunks, and the reader can copy an earlier
// fragment while the message is still open. So this writer cannot patch
// length fields later, as v1 does. It uses proto group encoding instead:
// - The format: proto_utils::kProtoGroupEndByte.
// - The service converts each packet back to length-delimited protobuf: see
//   ProtoRewriter.
//
// If the writer drops any part of a packet:
// - Its remaining bytes go to the drop buffer.
// - The next publication sets kFlagDataLoss. The reader discards all
//   published fragments in that chunk and reports the gap.
// - The service discards orphan continuations. It still delivers later
//   complete packets from chunks without the flag.
//
// Threads:
// - As with TraceWriterImpl, use each instance from one thread at a time.
class TraceWriterV2Impl : public TraceWriter,
                          public protozero::MessageFinalizationListener,
                          public protozero::ScatteredStreamWriter::Delegate {
 public:
  // ProducerRingBufferEndpoint::CreateTraceWriter() creates each writer.
  //
  // - |ring_buffer_endpoint|: packet bytes do not go through it. The writer
  //   calls it only for jobs that need the endpoint thread or shared state:
  //   - NotifyReader(), IsReaderAttached(): it is the delegate of
  //     |ring_buffer_writer_|. See SharedRingBufferWriter::Delegate.
  //   - Flush(): it asks for a drain, then runs the callback.
  //   - OnWriterDestroyed(): the destructor releases |id| through it.
  // - |ring_buffer|: the shared memory that receives the packets. Owned by
  //   |ring_buffer_endpoint|.
  // - |id|: the sequence ID of this writer. Reserved by
  //   |ring_buffer_endpoint|. While this writer holds it,
  //   |ring_buffer_endpoint| and |ring_buffer| stay alive.
  // - |target_buffer|: the service buffer for the packets. Stored in each
  //   chunk header.
  // - |policy|: what the writer does when the ring buffer is full. See
  //   BufferExhaustedPolicy.
  TraceWriterV2Impl(ProducerRingBufferEndpoint* ring_buffer_endpoint,
                    SharedRingBuffer* ring_buffer,
                    WriterID id,
                    BufferID target_buffer,
                    BufferExhaustedPolicy policy);
  ~TraceWriterV2Impl() override;

  TraceWriterV2Impl(const TraceWriterV2Impl&) = delete;
  TraceWriterV2Impl& operator=(const TraceWriterV2Impl&) = delete;
  TraceWriterV2Impl(TraceWriterV2Impl&&) = delete;
  TraceWriterV2Impl& operator=(TraceWriterV2Impl&&) = delete;

  // TraceWriter:
  TracePacketHandle NewTracePacket() override;
  void FinishTracePacket() override;
  // Unlike v1, |callback| does not wait for a service ack. It runs on the
  // endpoint thread after the drain request is sent.
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

  // Checks that no packet is open, before a new packet or a flush.
  // - A caller can call Message::Finalize() directly, without the handle.
  //   The message is then closed, but its last fragment is not published.
  //   This publishes it.
  // - Raw stream callers skip finalization. They must call
  //   FinishTracePacket() first, or the CHECK fails.
  void EnsurePacketClosed();

  // Publishes the open fragment, if any.
  // - |continues_on_next| sets kFlagContinuesOnNextChunk.
  // - If relocation fails, enters drop mode for the rest of the packet.
  void ClosePacketFragment(bool continues_on_next);

  // Sends the rest of the packet to |drop_buffer_| and returns its range.
  // On the first call for a packet, counts the drop and records the data
  // loss for the next publication.
  protozero::ContiguousMemoryRange EnterDropMode();

  // Receives Flush() and OnWriterDestroyed(). Also the delegate of
  // |ring_buffer_writer_|. See the constructor.
  ProducerRingBufferEndpoint* const ring_buffer_endpoint_;

  // Claims chunks and publishes fragments in the ring buffer.
  SharedRingBufferWriter ring_buffer_writer_;

  // Protozero writes packet bytes through this. It points into the open
  // fragment, or into |drop_buffer_| in drop mode.
  protozero::ScatteredStreamWriter stream_writer_;

  // Kept behind a pointer to avoid including the generated TracePacket header.
  // The same root message is reset and reused for every packet.
  std::unique_ptr<protozero::RootMessage<protos::pbzero::TracePacket>>
      cur_packet_;

  // Start of the open fragment in shared memory.
  // Null after the fragment closes, and in drop mode.
  uint8_t* fragment_begin_ = nullptr;

  // Drop mode writes go here, so the data source can finish the packet.
  // These bytes are never published.
  // - One buffer per writer. Writes use plain stores, so writers cannot
  //   share a buffer.
  // - Allocated on the first drop, then reused. Writers that never drop do
  //   not pay for it.
  // - Sized to the largest fragment. A raw stream caller can then reserve a
  //   contiguous range as large as in a normal fragment.
  std::vector<uint8_t> drop_buffer_;

  // True from NewTracePacket() until FinishTracePacket().
  bool packet_open_ = false;

  // True while writes go to |drop_buffer_|. Stays true until a later
  // NewTracePacket() gets a fragment in the ring buffer.
  bool in_drop_mode_ = false;

  // Sets first_packet_on_sequence on the first packet of this writer.
  bool first_packet_on_sequence_ = true;

  // Number of times this writer entered drop mode. Consecutive dropped
  // packets count once, as in TraceWriterImpl.
  uint64_t drop_count_ = 0;

  // PID of the process that created this writer. A DCHECK uses it to detect
  // a process fork during tracing, which is not supported.
  const base::PlatformProcessId process_id_;
};

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_V2_TRACE_WRITER_V2_IMPL_H_
