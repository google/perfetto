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

#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/protozero/message_handle.h"
#include "perfetto/protozero/root_message.h"
#include "perfetto/protozero/scattered_stream_writer.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/tracing/v2/shared_ring_buffer_writer.h"

namespace perfetto::tracing_v2 {

class SharedRingBuffer;
class SharedRingBufferArbiterImpl;

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
// Packet writes go directly to the ring buffer. The arbiter handles
// reader notifications, flush completion and WriterID retirement.
class TraceWriterV2Impl : public TraceWriter,
                          public protozero::MessageFinalizationListener,
                          public protozero::ScatteredStreamWriter::Delegate {
 public:
  TraceWriterV2Impl(SharedRingBufferArbiterImpl* arbiter,
                    SharedRingBuffer* ring,
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
  void Flush(std::function<void()> callback = {}) override;
  WriterID writer_id() const override { return ring_writer_.writer_id(); }
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

  SharedRingBufferArbiterImpl* const arbiter_;
  SharedRingBufferWriter ring_writer_;

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
  //
  // Allocated on first drop, then reused. Writers that never drop avoid the
  // extra memory. Sized to the chunk so raw stream callers can reserve a
  // contiguous range as large as a normal fragment.
  std::vector<uint8_t> drop_buffer_;

  bool packet_open_ = false;
  bool in_drop_mode_ = false;
  bool first_packet_on_sequence_ = true;

  uint64_t drop_count_ = 0;
};

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_V2_TRACE_WRITER_V2_IMPL_H_
