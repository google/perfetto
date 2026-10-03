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
#include <optional>

#include "perfetto/base/proc_utils.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/protozero/message_handle.h"
#include "perfetto/protozero/root_message.h"
#include "perfetto/protozero/scattered_stream_writer.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/tracing/v2/shared_ring_buffer_writer.h"

namespace perfetto::tracing_v2 {

class ProducerRingBufferArbiter;

namespace test {
class TraceWriterV2ImplTestPeer;
}  // namespace test

// TraceWriter implementation backed by a tracing v2 shared ring buffer.
//
// Data path from the SDK to the service. "v" is a call or a write. "^" is a
// read by the service. "<--" is an IPC request.
//
//          v1: SMB                           v2: ring buffer
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
//     |                                 | ProducerRingBufferArbiter.
//     v                                 v
//   [ SMB ]                           [ ring buffer ]
//     ^                                 ^
//     | copies the listed chunks        | copies all published chunks
//     |                                 |
//   service <-- CommitData IPC        service <-- DrainV2RingBuffer IPC
//               from the arbiter:                 from the ring buffer arbiter:
//               "chunks N, M are                  "read what is
//               complete"                         published"
//
// The ring buffer arbiter is ProducerRingBufferArbiter. It sends the drain
// requests of all writers of the ring buffer.
//
// Only this class calls both SharedRingBufferWriter and
// ProducerRingBufferArbiter:
// - SharedRingBufferWriter claims chunks, and publishes and relocates
//   fragments.
// - The arbiter delivers drain requests to the endpoint thread.
//
// This class decides:
// - When to ask for a drain:
//   - After a publication, if the ring buffer's outstanding positions reach
//     |drain_occupancy_threshold_|.
//   - After failed claims, if the writer drops the data. Only the reader can
//     move past those positions.
//   - Before each wait for space.
// - What to do when no chunk is free: wait for the reader and try again, or
//   drop the packet. See TryStallForSpace().
//
// A nested message can span chunks, and the reader can copy an earlier
// fragment while the message is still open. So this writer cannot patch
// length fields later, as v1 does. It uses proto group encoding instead:
// - The format: proto_utils::kProtoGroupEndByte.
// - The service converts each packet back to length-delimited protobuf: see
//   ProtoGroupRewriter.
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
  // ProducerRingBufferArbiter::CreateTraceWriter() creates each writer.
  //
  // |ring_buffer_arbiter| supplies the ring buffer. Packet bytes go directly
  // into it. The arbiter also receives drain requests and flushes. The
  // destructor releases |id| through it.
  //
  // - |id|: the arbiter allocates it from the shared WriterID pool. See the
  //   ownership diagram in ProducerRingBufferArbiter.
  // - |target_buffer|: the service buffer for the packets. Each chunk header
  //   stores it.
  // - |policy|: what the writer does when the ring buffer is full. See
  //   BufferExhaustedPolicy.
  TraceWriterV2Impl(ProducerRingBufferArbiter* ring_buffer_arbiter,
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
  // Unlike v1, |callback| does not wait for a service ack. See
  // ProducerRingBufferArbiter::Flush().
  void Flush(std::function<void()> callback = {}) override;
  WriterID writer_id() const override {
    return ring_buffer_writer_.writer_id();
  }
  uint64_t written() const override { return stream_writer_.written(); }
  uint64_t drop_count() const override { return drop_count_; }

 private:
  friend class test::TraceWriterV2ImplTestPeer;

  using ClockFunction = base::TimeMillis (*)();

  // protozero::MessageFinalizationListener:
  void OnMessageFinalized(protozero::Message*) override;

  // protozero::ScatteredStreamWriter::Delegate:
  protozero::ContiguousMemoryRange GetNewBuffer() override;
  uint8_t* AnnotatePatch(uint8_t*) override;

  // Checks that no packet is open, before a new packet or a flush.
  // - A caller can call Message::Finalize() directly, without the handle.
  //   The message is then closed, but its last fragment stays unpublished.
  //   This publishes it.
  // - Raw stream callers skip finalization. They must call
  //   FinishTracePacket() first, or the CHECK fails.
  void EnsurePacketClosed();

  // Obtains storage for the next fragment of the current packet: a new
  // fragment in the ring buffer, or the drop buffer. NewTracePacket() calls it
  // for the first fragment, GetNewBuffer() for each continuation.
  // - On success, returns the fragment's range and ends drop mode.
  // - If no chunk is free, applies the policy through TryStallForSpace(). If
  //   the policy drops the packet, enters drop mode and returns the drop
  //   buffer.
  protozero::ContiguousMemoryRange BeginPacketFragment(
      bool continues_from_prev);

  // Publishes the open fragment, if any.
  // - |continues_on_next| sets kFlagContinuesOnNextChunk.
  // - After the publication, asks for a drain at the threshold.
  // - If a relocation needs a new chunk, applies the policy through
  //   TryStallForSpace(), with one stall deadline for each replacement
  //   acquisition.
  // - If the policy drops the fragment, enters drop mode for the rest of the
  //   packet.
  // - Returns with no relocation pending in |ring_buffer_writer_|.
  void EndPacketFragment(bool continues_on_next);

  // Decides what to do after |ring_buffer_writer_| found no free chunk: wait
  // for the reader, or drop the data.
  //
  // Returns true after a wait. The caller then tries again to get a chunk.
  // Returns false, without a wait, if the writer must drop the data:
  // - The policy is kDrop.
  // - The policy is kStallThenDrop, and an earlier loss is still unreported.
  // - No reader is attached, so nothing frees space.
  // - The stall deadline passed. Under kStall, this aborts instead.
  //
  // |claim_failed|: the failed call returned kClaimFailed. Its reserved
  // positions stay unused until the reader moves past them. So a drop also
  // asks for a drain.
  //
  // |stall_deadline|: the deadline of the current chunk acquisition. Pass an
  // unset value on the first call for an acquisition, and the same value on
  // each retry. The first wait sets it.
  bool TryStallForSpace(bool claim_failed,
                        std::optional<base::TimeMillis>* stall_deadline);

  // Drops the rest of the open packet: its bytes go to GetDropBuffer() from now
  // on.
  // - If drop mode is not active yet, counts one drop and records the loss
  //   for the next publication.
  // - Drop mode lasts until a later packet gets a fragment in the ring buffer.
  void EnterDropMode();

  // Returns the process-wide drop buffer as a writable range. Only in drop
  // mode.
  protozero::ContiguousMemoryRange GetDropBuffer();

  // Receives drain requests, Flush() and OnWriterDestroyed(). See the
  // constructor.
  ProducerRingBufferArbiter* const ring_buffer_arbiter_;

  // Claims chunks and publishes fragments in the ring buffer.
  SharedRingBufferWriter ring_buffer_writer_;

  // What this writer does when the ring buffer has no free chunk.
  const BufferExhaustedPolicy buffer_exhausted_policy_;

  // A publication asks for a drain when the ring buffer has at least this many
  // outstanding positions.
  // - The count covers the positions of all writers.
  // - It includes reservations whose chunks are not published yet.
  // - The constructor computes the threshold from the ring buffer's capacity
  //   and the arbiter's drain_occupancy_percent().
  const uint32_t drain_occupancy_threshold_;

  // Time source for stall deadlines. Tests replace it to advance time
  // deterministically across acquisition attempts.
  ClockFunction get_time_ms_ = &base::GetWallTimeMs;

  // PID of the process that created this writer. A DCHECK uses it to detect
  // a process fork during tracing, which this writer does not support.
  const base::PlatformProcessId process_id_;

  // Protozero writes packet bytes through this. It points into the open
  // fragment, or into the drop buffer in drop mode.
  protozero::ScatteredStreamWriter stream_writer_;

  // A pointer, so this header does not need the generated TracePacket header.
  // NewTracePacket() resets and reuses the same root message for every packet.
  //
  // TODO(sashwinbalaji): Consider std::optional<RootMessage<TracePacket>>.
  // - It saves one heap allocation per writer, but needs the generated
  //   TracePacket header here.
  // - It waits because TraceWriterImpl makes the same choice. One decision
  //   must cover both writers.
  // - Revisit if a profile shows this allocation at writer creation.
  std::unique_ptr<protozero::RootMessage<protos::pbzero::TracePacket>>
      cur_packet_;

  // Start of the open fragment in shared memory.
  // Null after the fragment closes, and in drop mode.
  uint8_t* fragment_begin_ = nullptr;

  // True from NewTracePacket() until FinishTracePacket().
  bool packet_open_ = false;

  // True while writes go to the drop buffer. Stays true until a later
  // NewTracePacket() gets a fragment in the ring buffer.
  bool in_drop_mode_ = false;

  // Sets first_packet_on_sequence on the first packet of this writer.
  bool first_packet_on_sequence_ = true;

  // Number of times this writer entered drop mode. Consecutive dropped
  // packets count once, as in TraceWriterImpl.
  uint64_t drop_count_ = 0;
};

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_V2_TRACE_WRITER_V2_IMPL_H_
