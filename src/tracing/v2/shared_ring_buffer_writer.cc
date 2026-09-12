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

#include "src/tracing/v2/shared_ring_buffer_writer.h"

#include <stdint.h>
#include <string.h>

#include <optional>

#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"

namespace perfetto::tracing_v2 {
namespace {

// Stall ceiling: kStall aborts and kStallThenDrop starts dropping once a
// writer has waited this long. Matches v1's kAssertAtNStalls in
// SharedMemoryArbiterImpl::GetNewChunk(), roughly 30 seconds.
constexpr uint32_t kStallTimeoutMs = 30000;

}  // namespace

SharedRingBufferWriter::Delegate::~Delegate() = default;

SharedRingBufferWriter::SharedRingBufferWriter(
    SharedRingBuffer* ring,
    WriterID writer_id,
    BufferID target_buffer,
    BufferExhaustedPolicy buffer_exhausted_policy,
    Delegate* delegate)
    : ring_(ring),
      delegate_(delegate),
      writer_id_(writer_id),
      target_buffer_(target_buffer),
      buffer_exhausted_policy_(buffer_exhausted_policy),
      chunk_size_(ring->chunk_size()),
      max_fragment_size_(MaxFragmentSizeForEmptyChunk(chunk_size_)) {
  PERFETTO_CHECK(delegate_);
  // WriterIDs are nonzero, at most kMaxWriterID, and identify this writer
  // until all positions reserved under the id have been consumed.
  PERFETTO_DCHECK(writer_id_ != 0 && writer_id_ <= kMaxWriterID);
}

SharedRingBufferWriter::~SharedRingBufferWriter() {
  FinishCurrentChunk();
}

SharedRingBufferWriter::FragmentRange SharedRingBufferWriter::BeginFragment(
    uint32_t min_size,
    bool continues_from_prev) {
  PERFETTO_DCHECK(!has_open_fragment());
  // EndFragment(..., /*continues_on_next=*/true) drops the cached chunk, so a
  // continuation always starts without one. Reusing a cached chunk here would
  // silently lose the flag.
  PERFETTO_DCHECK(!continues_from_prev || !cur_chunk_);

  if (min_size > max_fragment_size_)
    return FragmentRange{BeginFragmentResult::kTooLarge};

  // Common case: we take our own Complete chunk back. We don't do that after
  // a loss, though: the next fragment has to land in a chunk that reports the
  // gap, and reusing this one would put post-loss data ahead of the flag.
  if (cur_chunk_) {
    PERFETTO_DCHECK(cur_chunk_state() == ChunkState::kComplete);
    const uint32_t available = MaxFragmentSizeInCurrentChunk();
    const bool can_append = !data_loss_pending_ && available >= min_size;
    if (can_append && ring_->TryReacquireChunkForWriting(
                          cur_chunk_idx_, expected_state_word_)) {
      expected_state_word_ =
          ReplaceChunkState(expected_state_word_, ChunkState::kBeingWritten);
      return BeginFragmentInCurrentChunk(available);
    }
    // The chunk is full, a loss is pending, or the reader reclaimed it.
    ResetCurrentChunk();
  }

  const uint32_t flags = continues_from_prev ? kFlagContinuesFromPrevChunk : 0u;
  const BeginFragmentResult result = AcquireNewChunk(flags);
  if (result != BeginFragmentResult::kSuccess)
    return FragmentRange{result};
  // The empty-chunk capacity already accounts for the header and size varint.
  return BeginFragmentInCurrentChunk(max_fragment_size_);
}

SharedRingBufferWriter::FragmentRange
SharedRingBufferWriter::BeginFragmentInCurrentChunk(uint32_t available) {
  PERFETTO_DCHECK(cur_chunk_);
  PERFETTO_DCHECK(cur_chunk_state() == ChunkState::kBeingWritten);
  PERFETTO_DCHECK(available > 0);
  cur_fragment_begin_ = payload_end_;
  return FragmentRange{/*result=*/BeginFragmentResult::kSuccess,
                       /*begin=*/cur_chunk_ + cur_fragment_begin_,
                       /*end=*/cur_chunk_ + cur_fragment_begin_ + available};
}

SharedRingBufferWriter::EndFragmentResult SharedRingBufferWriter::EndFragment(
    uint32_t size,
    bool continues_on_next) {
  PERFETTO_DCHECK(has_open_fragment());
  PERFETTO_DCHECK(cur_chunk_);
  PERFETTO_DCHECK(cur_chunk_state() == ChunkState::kBeingWritten);
  // |size| must still fit in the range BeginFragment() handed out, together
  // with the size varint that encodes it.
  PERFETTO_DCHECK(size <= MaxFragmentSizeForAvailableBytes(
                              sizes_begin_ - cur_fragment_begin_));

  // Nothing becomes visible until ReleaseCurrentChunkAsComplete().
  uint8_t* const new_sizes_begin_ptr =
      WriteFragmentSize(cur_chunk_ + sizes_begin_, size);
  // sizes_begin_ stores a byte offset. The helper returns a pointer.
  // Subtract the chunk's start pointer to get the new offset.
  sizes_begin_ = static_cast<uint32_t>(new_sizes_begin_ptr - cur_chunk_);
  payload_end_ = cur_fragment_begin_ + size;
  ++num_fragments_;
  cur_fragment_begin_ = kNoFragmentOpen;

  return ReleaseCurrentChunkAsComplete(size, continues_on_next);
}

SharedRingBufferWriter::EndFragmentResult
SharedRingBufferWriter::FinishCurrentChunk() {
  // The open fragment is not in the published count. Abandon its bytes.
  // Previously published fragments stay visible.
  cur_fragment_begin_ = kNoFragmentOpen;

  EndFragmentResult result = EndFragmentResult::kSuccess;
  if (cur_chunk_ && cur_chunk_state() == ChunkState::kBeingWritten) {
    result = ReleaseCurrentChunkAsComplete(std::nullopt,
                                           /*continues_on_next=*/false);
  }
  ResetCurrentChunk();
  return result;
}

uint32_t SharedRingBufferWriter::MaxFragmentSizeInCurrentChunk() const {
  PERFETTO_DCHECK(cur_chunk_);
  if (num_fragments_ >= kMaxFragmentsPerChunk)
    return 0;
  if (sizes_begin_ <= payload_end_)
    return 0;
  const uint32_t available_bytes = sizes_begin_ - payload_end_;
  return MaxFragmentSizeForAvailableBytes(available_bytes);
}

SharedRingBufferWriter::BeginFragmentResult
SharedRingBufferWriter::AcquireNewChunk(uint32_t continuation_flags) {
  PERFETTO_DCHECK(!cur_chunk_);

  // A data-loss flag reaches the new chunk through either:
  // - data_loss_pending_: the packet writer recorded a loss.
  // - continuation_flags: the reader requested a rewrite before any fragment
  //   was published. The flag travels with the relocated fragment.
  //
  // In the second case data_loss_pending_ is already clear. Use the combined
  // |flags| below to decide whether this chunk reports loss.
  const uint32_t flags =
      continuation_flags | (data_loss_pending_ ? kFlagDataLoss : 0u);
  const uint32_t being_written_word =
      MakeDataStateWord(ChunkState::kBeingWritten, ChunkFormat::kTargetBuffer,
                        flags, 0, writer_id_);

  // kStallThenDrop waits only on the first exhaustion of a drop episode. While
  // the loss is still unreported, that is while |flags| carries kFlagDataLoss,
  // every further probe uses kDrop so that each dropped packet does not cost
  // another timeout. Publishing a chunk with the flag ends the episode, and a
  // later exhaustion stalls again. This matches v1's drop_packets_ behaviour.
  BufferExhaustedPolicy policy = buffer_exhausted_policy_;
  if (policy == BufferExhaustedPolicy::kStallThenDrop &&
      (flags & kFlagDataLoss)) {
    policy = BufferExhaustedPolicy::kDrop;
  }

  std::optional<base::TimeMillis> stall_deadline;

  // Each attempt reserves a position, then tries to claim its physical chunk:
  //
  // 1. TryReserveWritePos() finds the ring buffer full.
  //    - write_pos stays unchanged. No position was reserved.
  //    - Apply the buffer-exhaustion policy below.
  //
  // 2. Reservation and TryAcquireChunkForWriting() both succeed.
  //    - This writer now owns the chunk. Return it to the caller.
  //
  // 3. Reservation succeeds, but TryAcquireChunkForWriting() fails.
  //    - The reader advanced the wrap count before we claimed the chunk.
  //      We may have been descheduled after advancing write_pos.
  //    - Or the chunk still belongs to an older reservation:
  //      - Its writer claimed the chunk, then got descheduled.
  //      - The reader copied its published fragments and set RewriteRequested.
  //      - The reader advanced read_pos. It did not release the chunk.
  //      - Advancing read_pos made room for another reservation.
  //        TryReserveWritePos() checks the positions, not the chunk state.
  //      - Our new reservation selects that same physical chunk.
  //        The claim fails because the older writer has not acknowledged yet.
  //    - Leave the reservation unclaimed. Try a new reservation with continue.
  //    - After num_chunks failed claims, apply the exhaustion policy instead.

  // num_chunks limits failed claims in one round of attempts.
  // - Other writers can reserve positions between our attempts.
  // - Our reservations can select the same physical chunk on different wraps.
  // - Reaching the limit does not mean we checked every physical chunk.
  uint32_t num_failed_claims = 0;
  bool saw_unclaimable_chunk = false;
  for (;;) {
    const auto reservation = ring_->TryReserveWritePos();

    if (reservation.result == SharedRingBuffer::ReserveResult::kReserved) {
      // Happy case: the reserved position's chunk is Free for this traversal.
      if (ring_->TryAcquireChunkForWriting(reservation.chunk_pos,
                                           being_written_word)) {
        cur_chunk_idx_ = ChunkIndex::FromPosition(reservation.chunk_pos,
                                                  ring_->num_chunks());
        cur_chunk_ = ring_->chunk_at(cur_chunk_idx_);
        expected_state_word_ = being_written_word;
        payload_end_ = kTargetBufferPayloadOffset;
        sizes_begin_ = chunk_size_;
        num_fragments_ = 0;
        data_loss_pending_ = false;
        StoreTargetBufferId(cur_chunk_, target_buffer_);
        return BeginFragmentResult::kSuccess;
      }

      // write_pos advanced, but this reservation could not claim its chunk.
      // - We may have been descheduled before claiming the chunk. The reader
      //   could then advance its wrap count before we resumed.
      // - The reserved position fixes both the chunk index and the wrap count.
      // - A Free word with a different wrap count prepares this chunk for a
      //   later position. We have not reserved that position. Do not change
      //   the expected wrap count and retry the claim.
      // - Call TryReserveWritePos() again. Other writers may have reserved the
      //   intervening positions, so we may not get the next position.
      // - Use the chunk index and wrap count from that new reservation.
      //   Do not just move to the next physical chunk.

      // See "Chunk ownership and wrap identity" in shared_ring_buffer_abi.h
      // for the two failure cases and the reader's state updates.
      ++stats_.failed_claims;
      saw_unclaimable_chunk = true;
      if (++num_failed_claims < ring_->num_chunks())
        continue;
    }

    // If we got here, either:
    // - TryReserveWritePos() found no space in the ring buffer.
    //   It left write_pos unchanged.
    // - TryAcquireChunkForWriting() failed num_chunks times this round.
    //   Each reservation advanced write_pos but left its chunk unclaimed.
    //   Earlier failures took continue to try another reservation.
    //   This failure reached the attempt limit.

    // Notify the reader after failed claims or before waiting.
    // Reservation can find the ring buffer full after only a few failed
    // claims. The reader may still need to consume those unclaimed
    // reservations.
    if (num_failed_claims != 0 || policy != BufferExhaustedPolicy::kDrop) {
      delegate_->NotifyReader();
      num_failed_claims = 0;
    }

    // Classify the exhaustion.
    // Preserve a failed claim across waits, even if the last reservation
    // found the ring buffer full: this acquisition has already left unclaimed
    // reservations behind.
    const BeginFragmentResult exhausted_result =
        saw_unclaimable_chunk ? BeginFragmentResult::kNoChunkAvailable
                              : BeginFragmentResult::kFull;

    if (policy == BufferExhaustedPolicy::kDrop) {
      PERFETTO_DLOG("tracing v2: writer %u: %s: returning without a chunk",
                    writer_id_,
                    saw_unclaimable_chunk ? "no chunk could be claimed"
                                          : "ring buffer full");
      return exhausted_result;
    }

    // The deadline starts on the first exhaustion and spans every wait.
    const base::TimeMillis now = base::GetWallTimeMs();
    if (!stall_deadline)
      stall_deadline = now + base::TimeMillis(kStallTimeoutMs);
    if (now >= *stall_deadline) {
      if (policy == BufferExhaustedPolicy::kStall) {
        PERFETTO_FATAL(
            "tracing v2: writer %u could not acquire a chunk for %u ms: "
            "possible deadlock",
            writer_id_, kStallTimeoutMs);
      }
      PERFETTO_DLOG(
          "tracing v2: writer %u stalled for %u ms: returning without a "
          "chunk",
          writer_id_, kStallTimeoutMs);
      return exhausted_result;
    }

    // Sleep until read_pos moves past the value the last reservation attempt
    // sampled. The loop rechecks capacity after any return, including a
    // timeout or a spurious wake.
    const uint32_t timeout_ms =
        static_cast<uint32_t>((*stall_deadline - now).count());
    const SharedRingBuffer::WriterWaitResult wait =
        ring_->WaitForReadPosChange(reservation.read_pos_for_wait, timeout_ms);

    // Without a wait primitive kStall cannot be honoured, and kStallThenDrop
    // drops at once.
    if (wait == SharedRingBuffer::WriterWaitResult::kUnavailable) {
      if (policy == BufferExhaustedPolicy::kStall) {
        PERFETTO_FATAL(
            "tracing v2: writer %u cannot stall because waiting on read_pos "
            "is unavailable",
            writer_id_);
      }
      PERFETTO_DLOG(
          "tracing v2: writer %u cannot wait on read_pos: returning without "
          "a chunk",
          writer_id_);
      return exhausted_result;
    }
  }
}

SharedRingBufferWriter::EndFragmentResult
SharedRingBufferWriter::ReleaseCurrentChunkAsComplete(
    std::optional<uint32_t> fragment_size,
    bool continues_on_next) {
  PERFETTO_DCHECK(cur_chunk_);
  PERFETTO_DCHECK(cur_chunk_state() == ChunkState::kBeingWritten);
  PERFETTO_DCHECK(!has_open_fragment());

  // EndFragment publishes before another fragment can begin. A rewrite leaves
  // only the just-ended fragment to relocate. FinishCurrentChunk abandons the
  // open fragment, so it has nothing to relocate.
  //
  //   BeingWritten(N) -- writer --> Complete(N + has_fragment)
  //          |
  //          +--------- reader --> RewriteRequested(N)
  //                                         |
  //                    writer copies the fragment and acknowledges
  //
  // The replacement starts at BeingWritten(0). It can lose the same race.
  // Keep looping with the same fragment_size until publication or drop.
  for (;;) {
    uint32_t flags = PayloadFlagsOf(expected_state_word_);
    if (continues_on_next)
      flags |= kFlagContinuesOnNextChunk;
    const uint32_t complete_word =
        MakeDataStateWord(ChunkState::kComplete, ChunkFormat::kTargetBuffer,
                          flags, num_fragments_, writer_id_);

    uint32_t expected = expected_state_word_;
    // Happy case: the reader has not requested a rewrite of this chunk.
    if (ring_->TryReleaseChunkAsComplete(cur_chunk_idx_, complete_word,
                                         &expected)) {
      expected_state_word_ = complete_word;
      // Let go of the chunk when the next fragment must start elsewhere:
      //  - continues_on_next: kFlagContinuesOnNextChunk describes the last
      //    fragment. Appending another would make it describe the wrong one,
      //    and a partial chunk read could then end in the middle of a packet.
      //  - The fragment count is at its maximum.
      //  - At most one byte is left between the payload and the sizes.
      //    BeginFragment() hands out a non-empty range, and the size varint
      //    needs one more byte, so a one-byte gap cannot be reused even by a
      //    zero-length fragment. Two bytes can. EndFragment() only accepts a
      //    fragment whose payload and size entry fit, so payload_end_ <=
      //    sizes_begin_ and the subtraction cannot underflow.
      if (continues_on_next || num_fragments_ >= kMaxFragmentsPerChunk ||
          sizes_begin_ - payload_end_ <= 1) {
        ResetCurrentChunk();
      }
      return EndFragmentResult::kSuccess;
    }

    // Only the reader's rewrite request may beat this publication.
    if (ChunkStateOf(expected) != ChunkState::kRewriteRequested ||
        WriterIDOf(expected) != writer_id_) {
      PERFETTO_FATAL(
          "tracing v2: publication of chunk %u by writer %u lost to state word "
          "0x%08x, which is not a rewrite request for this writer",
          cur_chunk_idx_.value(), writer_id_, expected);
    }

    // expected is RewriteRequested. The reader did a partial chunk read:
    // - It consumed NumFragmentsOf(expected) published fragments.
    // - We were still writing the next fragment, so it did not read that one.
    // - It moved on. We must publish any remaining fragment in a new chunk.
    const uint32_t num_fragments_read = NumFragmentsOf(expected);

    // The reader consumed exactly the fragments we had already published.
    // Its rewrite request preserves that count in the state word.
    PERFETTO_DCHECK(num_fragments_read == NumFragmentsOf(expected_state_word_));

    // Any extra fragment in num_fragments_ still needs to be published:
    // - EndFragment() counted the just-ended fragment before trying to publish.
    // - FinishCurrentChunk() abandoned the open fragment without counting it.
    // Another fragment cannot begin until EndFragment() finishes, so at most
    // one fragment remains unpublished.
    PERFETTO_DCHECK(num_fragments_ ==
                    num_fragments_read + (fragment_size.has_value() ? 1u : 0u));

    // Save the unpublished fragment before acknowledging. The reader can
    // reclaim the old chunk after that. Acknowledge before waiting for space
    // so we do not pin the old chunk while waiting for a new one.
    if (fragment_size) {
      // TODO(sashwinbalaji): Measure first-relocation latency and retained
      // memory per writer before deciding whether to preallocate this scratch
      // or release it after use.
      relocation_payload_.assign(cur_chunk_ + payload_end_ - *fragment_size,
                                 cur_chunk_ + payload_end_);
    }

    if (!ring_->TryAcknowledgeRewrite(cur_chunk_idx_, expected)) {
      PERFETTO_FATAL(
          "tracing v2: writer %u could not acknowledge chunk %u: only its "
          "owner may leave RewriteRequested",
          writer_id_, cur_chunk_idx_.value());
    }
    ++stats_.relocations;

    // Chunk-start flags move only when the reader took no fragments.
    const uint32_t inherited_flags =
        PayloadFlagsOf(expected) &
        (kFlagContinuesFromPrevChunk | kFlagDataLoss);
    const uint32_t relocated_flags =
        num_fragments_read == 0 ? inherited_flags : 0;

    ResetCurrentChunk();

    if (!fragment_size) {
      // Nothing remains to relocate. A loss flag the reader did not take has
      // no fragment to travel with, so the next chunk must carry it instead.
      PERFETTO_DCHECK(!continues_on_next);
      if (relocated_flags & kFlagDataLoss)
        data_loss_pending_ = true;
      return EndFragmentResult::kSuccess;
    }

    if (AcquireNewChunk(relocated_flags) != BeginFragmentResult::kSuccess) {
      PERFETTO_DLOG(
          "tracing v2: writer %u dropped one relocated fragment: no "
          "replacement chunk",
          writer_id_);
      ++stats_.fragments_dropped;
      data_loss_pending_ = true;
      return EndFragmentResult::kRelocationDropped;
    }

    // Restore the one fragment, which fits in a chunk of the same size.
    if (!relocation_payload_.empty()) {
      memcpy(cur_chunk_ + kTargetBufferPayloadOffset,
             relocation_payload_.data(), relocation_payload_.size());
    }
    payload_end_ = kTargetBufferPayloadOffset + *fragment_size;
    uint8_t* sizes_begin =
        WriteFragmentSize(cur_chunk_ + chunk_size_, *fragment_size);
    sizes_begin_ = static_cast<uint32_t>(sizes_begin - cur_chunk_);
    num_fragments_ = 1;
    // Round again to publish the replacement, which the reader may also scrape.
  }
}

void SharedRingBufferWriter::ResetCurrentChunk() {
  cur_chunk_ = nullptr;
  cur_chunk_idx_ = ChunkIndex::FromIndex(0);
  expected_state_word_ = 0;
  payload_end_ = 0;
  sizes_begin_ = 0;
  num_fragments_ = 0;
  cur_fragment_begin_ = kNoFragmentOpen;
}

}  // namespace perfetto::tracing_v2
