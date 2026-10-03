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

#include "src/tracing/v2/trace_writer_v2_impl.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/base/proc_utils.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/thread_annotations.h"
#include "perfetto/protozero/message.h"
#include "perfetto/protozero/proto_utils.h"
#include "src/tracing/v2/producer_ring_buffer_arbiter.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::tracing_v2 {
namespace {

// A new fragment must fit Protozero's largest scalar field.
constexpr uint32_t kMinFragmentPayloadSize =
    static_cast<uint32_t>(protozero::proto_utils::kMaxSimpleFieldEncodedSize);

// Each wait for space requests at most this time. A wait can end early, for
// example when the reader moves read_pos.
// SharedRingBufferWriter::WaitForReadPosChange() states the exact guarantees.
//
// The limit makes the writer check again also without reader progress:
// - If the reader detached during the wait, the writer drops the data. It
//   does not wait until the stall deadline and crash.
// - If the last drain did not free space, the writer asks for another one.
//
// The 100 ms limit matches the longest sleep in
// SharedMemoryArbiterImpl::GetNewChunk().
constexpr uint32_t kMaxWaitMs = 100;

// After this time, kStall aborts and kStallThenDrop drops the data.
// - The time starts when one chunk acquisition first needs to wait.
// - It covers all failed claim rounds and waits of that acquisition.
// - It does not limit the time to finish a fragment: a relocation can claim
//   several replacements, each with its own deadline.
//
// The 30-second limit matches v1's approximate timeout from kAssertAtNStalls
// in SharedMemoryArbiterImpl::GetNewChunk().
constexpr uint32_t kStallTimeoutMs = 30000;

// Drop mode writes go here, so the data source can finish the packet. Nobody
// reads it.
// - All writers of the process share it, as v1 writers share
//   g_garbage_chunk. Their concurrent writes are a benign race.
// - Sized to the largest chunk. A raw stream caller can then request a
//   contiguous range as large as in a normal fragment.
uint8_t g_drop_buffer[kMaxChunkSize];

}  // namespace

TraceWriterV2Impl::TraceWriterV2Impl(
    ProducerRingBufferArbiter* ring_buffer_arbiter,
    WriterID id,
    BufferID target_buffer,
    BufferExhaustedPolicy policy)
    : ring_buffer_arbiter_(ring_buffer_arbiter),
      ring_buffer_writer_(ring_buffer_arbiter->ring_buffer(),
                          id,
                          target_buffer),
      buffer_exhausted_policy_(policy),
      process_id_(base::GetProcessId()),
      stream_writer_(this),
      cur_packet_(std::make_unique<
                  protozero::RootMessage<protos::pbzero::TracePacket>>()) {}

TraceWriterV2Impl::~TraceWriterV2Impl() {
  // Flush implicitly, as TraceWriterBase documents: publish the last packet
  // and ask for a drain. A publication alone makes the bytes readable, but it
  // does not make the service read them.
  //
  // Both happen before the arbiter releases this WriterID:
  // - The release can let the endpoint destroy the arbiter.
  // - Ring buffer reservations keep the order if another ring buffer writer
  //   reuses the ID. Nothing handles reuse by a v1 writer yet. See the TODO
  //   in ProducerRingBufferArbiter::OnWriterDestroyed().
  FinishTracePacket();
  Flush();
  ring_buffer_arbiter_->OnWriterDestroyed(writer_id());
}

TraceWriter::TracePacketHandle TraceWriterV2Impl::NewTracePacket() {
  PERFETTO_DCHECK(process_id_ == base::GetProcessId());
  EnsurePacketClosed();

  stream_writer_.Reset(BeginPacketFragment(/*continues_from_prev=*/false));
  cur_packet_->Reset(&stream_writer_,
                     protozero::Message::Encoding::kProtoGroup);
  packet_open_ = true;
  if (PERFETTO_UNLIKELY(first_packet_on_sequence_)) {
    cur_packet_->set_first_packet_on_sequence(true);
    first_packet_on_sequence_ = false;
  }

  TracePacketHandle handle(cur_packet_.get());
  handle.set_finalization_listener(this);
  return handle;
}

void TraceWriterV2Impl::FinishTracePacket() {
  PERFETTO_DCHECK(process_id_ == base::GetProcessId());
  if (!packet_open_)
    return;

  // Finalize() can append closing bytes that cross a chunk boundary. Keep
  // packet_open_ true until it returns so GetNewBuffer() can obtain space for
  // those bytes. Then publish the final fragment without a continuation flag.
  cur_packet_->Finalize();
  packet_open_ = false;
  EndPacketFragment(/*continues_on_next=*/false);
}

void TraceWriterV2Impl::Flush(std::function<void()> callback) {
  EnsurePacketClosed();

  // A completed fragment leaves its chunk cached for later packets. Release
  // that chunk before ring_buffer_arbiter_->Flush() so later packets use new
  // reservations.
  ring_buffer_writer_.FinishCurrentChunk();
  stream_writer_.Reset({nullptr, nullptr});
  ring_buffer_arbiter_->Flush(std::move(callback));
}

void TraceWriterV2Impl::OnMessageFinalized(protozero::Message*) {
  FinishTracePacket();
}

protozero::ContiguousMemoryRange TraceWriterV2Impl::GetNewBuffer() {
  if (!packet_open_) {
    // A stale handle can request space without an open packet. This caller
    // error violates the data-source API contract.
    PERFETTO_DFATAL("TraceWriterV2Impl: write outside an open packet");
    EnterDropMode();
    return GetDropBuffer();
  }

  // If the writer loses part of a packet, the remaining bytes are not useful.
  // Send the rest to the drop buffer. Try the ring buffer for the next packet.
  if (in_drop_mode_)
    return GetDropBuffer();

  // The current range cannot satisfy the stream's request. Publish this
  // fragment with kFlagContinuesOnNextChunk and continue in another chunk.
  EndPacketFragment(/*continues_on_next=*/true);

  // EndPacketFragment() can fail to relocate after the reader requests a
  // rewrite. Then the rest of the packet goes to the drop buffer, not to a
  // continuation fragment.
  if (in_drop_mode_)
    return GetDropBuffer();

  return BeginPacketFragment(/*continues_from_prev=*/true);
}

uint8_t* TraceWriterV2Impl::AnnotatePatch(uint8_t*) {
  PERFETTO_FATAL("TraceWriterV2Impl cannot patch previously written bytes");
}

void TraceWriterV2Impl::EnsurePacketClosed() {
  if (packet_open_ && cur_packet_->is_finalized())
    FinishTracePacket();
  PERFETTO_CHECK(!packet_open_);
}

protozero::ContiguousMemoryRange TraceWriterV2Impl::BeginPacketFragment(
    bool continues_from_prev) {
  std::optional<base::TimeMillis> stall_deadline;
  for (;;) {
    const auto range = ring_buffer_writer_.BeginFragment(
        kMinFragmentPayloadSize, continues_from_prev);
    if (range.result == SharedRingBufferWriter::BeginFragmentResult::kSuccess) {
      fragment_begin_ = range.begin;
      in_drop_mode_ = false;
      return {range.begin, range.end};
    }
    // Every valid chunk size holds kMinFragmentPayloadSize.
    PERFETTO_DCHECK(range.result !=
                    SharedRingBufferWriter::BeginFragmentResult::kTooLarge);

    const bool claim_failed =
        range.result ==
        SharedRingBufferWriter::BeginFragmentResult::kClaimFailed;
    if (!TryStallForSpace(claim_failed, &stall_deadline)) {
      EnterDropMode();
      return GetDropBuffer();
    }
  }
}

void TraceWriterV2Impl::EndPacketFragment(bool continues_on_next) {
  if (!fragment_begin_) {
    // The packet went to the drop buffer. The next publication carries
    // kFlagDataLoss, even if it reuses a cached chunk.
    return;
  }

  const uint32_t used =
      static_cast<uint32_t>(stream_writer_.write_ptr() - fragment_begin_);
  fragment_begin_ = nullptr;

  // If the reader took the chunk, EndFragment() relocates the fragment and
  // needs a replacement chunk. See SharedRingBufferWriter::EndFragment().
  //
  // If EndFragment() cannot claim a replacement, the relocation stays pending
  // until the policy lets the writer try again or drop it.
  SharedRingBufferWriter::EndFragmentResult result =
      ring_buffer_writer_.EndFragment(used, continues_on_next);
  // The stall deadline of the current replacement acquisition. It starts at
  // the first wait after EndFragment() returns.
  std::optional<base::TimeMillis> stall_deadline;
  while (result != SharedRingBufferWriter::EndFragmentResult::kSuccess) {
    const bool claim_failed =
        result == SharedRingBufferWriter::EndFragmentResult::kClaimFailed;
    if (!TryStallForSpace(claim_failed, &stall_deadline)) {
      // Discard the saved fragment and the rest of the packet.
      ring_buffer_writer_.DropRelocation();
      EnterDropMode();
      return;
    }
    const auto retry = ring_buffer_writer_.RetryRelocation();
    // A claimed replacement ends the current acquisition, even if the reader
    // then took it before publication. The next acquisition gets its own
    // deadline at its first wait. Retries without a claim keep the deadline.
    if (retry.acquired_replacement)
      stall_deadline.reset();
    result = retry.result;
  }

  // Batch drain requests until occupancy reaches the threshold:
  // - This reduces task and IPC traffic when packets arrive separately.
  // - It lets this writer reuse its cached Complete chunk for longer. A drain
  //   frees that chunk, and the next fragment then needs a new one.
  const uint32_t outstanding_positions =
      ring_buffer_arbiter_->ring_buffer()->LoadNumOutstandingPositionsRelaxed();
  if (outstanding_positions >=
      ring_buffer_arbiter_->drain_occupancy_threshold()) {
    ring_buffer_arbiter_->RequestDrain(
        ProducerRingBufferArbiter::DrainUrgency::kRoutine);
  }
}

bool TraceWriterV2Impl::TryStallForSpace(
    bool claim_failed,
    std::optional<base::TimeMillis>* stall_deadline) {
  // While a loss waits for its report, kStallThenDrop acts as kDrop.
  // Otherwise each dropped packet can cost another full timeout.
  //
  // A later
  // acquisition can stall again after a chunk carries the loss. This matches
  // v1's drop_packets_ behavior.
  bool can_stall = buffer_exhausted_policy_ != BufferExhaustedPolicy::kDrop;
  if (buffer_exhausted_policy_ == BufferExhaustedPolicy::kStallThenDrop &&
      ring_buffer_writer_.has_pending_data_loss()) {
    can_stall = false;
  }

  // Time left before the stall deadline. Zero means drop.
  // Without an attached reader, nothing frees space. So drop, also under
  // kStall. A wait would only end at the deadline, where kStall crashes.
  uint32_t time_left_ms = 0;
  if (can_stall && ring_buffer_arbiter_->IsReaderAttached()) {
    const base::TimeMillis now = get_time_ms_();
    if (!stall_deadline->has_value())
      *stall_deadline = now + base::TimeMillis(kStallTimeoutMs);
    const base::TimeMillis deadline = **stall_deadline;
    if (now < deadline) {
      time_left_ms = static_cast<uint32_t>((deadline - now).count());
    } else if (buffer_exhausted_policy_ == BufferExhaustedPolicy::kStall) {
      PERFETTO_FATAL(
          "tracing v2: writer %u could not acquire a chunk for %u ms: "
          "possible deadlock",
          writer_id(), kStallTimeoutMs);
    }
  }

  // Every drop takes this exit.
  if (time_left_ms == 0) {
    // Failed claims left reserved positions that only the reader can move
    // past. Until it does, they block the reservations of all writers. An
    // earlier drain does not cover positions reserved after it.
    if (claim_failed) {
      ring_buffer_arbiter_->RequestDrain(
          ProducerRingBufferArbiter::DrainUrgency::kRoutine);
    }
    PERFETTO_DLOG(
        "tracing v2: writer %u: %s: dropping data", writer_id(),
        claim_failed ? "no chunk could be claimed" : "ring buffer full");
    return false;
  }

  // Make the reader run before the wait. Its drain frees space, and moves
  // read_pos past the positions of failed claims.
  ring_buffer_arbiter_->RequestDrain(
      ProducerRingBufferArbiter::DrainUrgency::kUrgent);

  // With a futex, the wait compares read_pos with the value that the last
  // reservation attempt saw, before the request above. So it also sees reader
  // progress that happens between the request and the wait.
  //
  // The requested wait time comes from the time left before the deadline,
  // measured before the request above. So the wait can end slightly after the
  // deadline. The next check then drops or aborts.
  ring_buffer_writer_.WaitForReadPosChange(std::min(kMaxWaitMs, time_left_ms));
  return true;
}

void TraceWriterV2Impl::EnterDropMode() {
  fragment_begin_ = nullptr;
  // Consecutive dropped packets count as one drop.
  if (in_drop_mode_)
    return;
  in_drop_mode_ = true;
  ++drop_count_;
  ring_buffer_writer_.RecordDataLoss();
}

protozero::ContiguousMemoryRange TraceWriterV2Impl::GetDropBuffer() {
  PERFETTO_DCHECK(in_drop_mode_);
  PERFETTO_ANNOTATE_BENIGN_RACE_SIZED(&g_drop_buffer, sizeof(g_drop_buffer),
                                      "nobody reads the drop buffer")
  return {&g_drop_buffer[0], &g_drop_buffer[0] + sizeof(g_drop_buffer)};
}

}  // namespace perfetto::tracing_v2
