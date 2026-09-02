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
  // until all positions reserved under the id have been resolved.
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
  uint8_t* sizes_begin = cur_chunk_ + sizes_begin_;
  sizes_begin = WriteFragmentSize(sizes_begin, size);
  sizes_begin_ = static_cast<uint32_t>(sizes_begin - cur_chunk_);
  payload_end_ = cur_fragment_begin_ + size;
  ++num_fragments_;
  cur_fragment_begin_ = kNoFragmentOpen;

  return ReleaseCurrentChunkAsComplete(size, continues_on_next);
}

SharedRingBufferWriter::EndFragmentResult
SharedRingBufferWriter::FinishCurrentChunk() {
  // The open fragment is not in the published count. Abandon its bytes;
  // previously published fragments stay visible.
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

  // The flags of the chunk about to be claimed. A data-loss flag reaches this
  // point in one of two ways: the packet writer recorded a loss
  // (data_loss_pending_), or the reader scraped an empty prefix and the flag is
  // travelling with the relocated suffix (continuation_flags). In the second
  // case data_loss_pending_ is already clear, so |flags| is the one value that
  // says whether this chunk reports a loss.
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

  // Each round reserves a position and then claims its chunk.
  //
  // 1. If the ring is full, no position is reserved; apply the exhaustion
  //    policy below.
  // 2. If both reservation and claim succeed, return the chunk.
  // 3. A failed claim leaves a hole; reserve a later position instead.
  //
  // Stop when the ring reports full or after num_chunks failed claims. The
  // latter bounds attempts, not physical chunks visited: other writers can
  // take intervening positions, so repeated attempts may hit the same pinned
  // chunk.
  uint32_t num_failed_claims = 0;
  bool saw_unclaimable_chunk = false;
  for (;;) {
    const auto reservation = ring_->TryReserveWritePos();

    if (reservation.result == SharedRingBuffer::ReserveResult::kReserved) {
      // Happy case: the reserved position's chunk is Free for this traversal.
      if (ring_->TryAcquireChunkForWriting(reservation.position,
                                           being_written_word)) {
        cur_chunk_idx_ =
            ChunkIndexOf(reservation.position, ring_->num_chunks());
        cur_chunk_ = ring_->chunk_at(cur_chunk_idx_);
        expected_state_word_ = being_written_word;
        payload_end_ = kTargetBufferPayloadOffset;
        sizes_begin_ = chunk_size_;
        num_fragments_ = 0;
        data_loss_pending_ = false;
        StoreTargetBufferID(cur_chunk_, target_buffer_);
        return BeginFragmentResult::kSuccess;
      }

      // This reservation is now a hole. Never retry it against a different
      // Free word; reserve a later position instead.
      ++stats_.failed_claims;
      saw_unclaimable_chunk = true;
      if (++num_failed_claims < ring_->num_chunks())
        continue;
    }

    // The reader resolves holes and moves read_pos, so notify it whenever
    // holes were created or this writer is about to wait. The count can be
    // below num_chunks here: a later reservation can find the ring full after
    // only some failed claims, and those holes still need the notification.
    if (num_failed_claims != 0 || policy != BufferExhaustedPolicy::kDrop) {
      delegate_->NotifyReader();
      num_failed_claims = 0;
    }

    // Classify the exhaustion.
    // Preserve a failed claim across waits, even if the last reservation
    // found the ring full: this acquisition has already left holes behind.
    const BeginFragmentResult exhausted_result =
        saw_unclaimable_chunk ? BeginFragmentResult::kNoChunkAvailable
                              : BeginFragmentResult::kFull;

    if (policy == BufferExhaustedPolicy::kDrop) {
      PERFETTO_DLOG(
          "tracing v2: writer %u: %s; returning without a chunk", writer_id_,
          saw_unclaimable_chunk ? "no chunk could be claimed" : "ring full");
      return exhausted_result;
    }

    // The deadline starts on the first exhaustion and spans every wait.
    const base::TimeMillis now = base::GetWallTimeMs();
    if (!stall_deadline)
      stall_deadline = now + base::TimeMillis(kStallTimeoutMs);
    if (now >= *stall_deadline) {
      if (policy == BufferExhaustedPolicy::kStall) {
        PERFETTO_FATAL(
            "tracing v2: writer %u could not acquire a chunk for %u ms; "
            "possible deadlock",
            writer_id_, kStallTimeoutMs);
      }
      PERFETTO_DLOG(
          "tracing v2: writer %u stalled for %u ms; returning without a "
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
          "tracing v2: writer %u cannot wait on read_pos; returning without "
          "a chunk",
          writer_id_);
      return exhausted_result;
    }
  }
}

SharedRingBufferWriter::EndFragmentResult
SharedRingBufferWriter::ReleaseCurrentChunkAsComplete(
    std::optional<uint32_t> suffix_size,
    bool continues_on_next) {
  PERFETTO_DCHECK(cur_chunk_);
  PERFETTO_DCHECK(cur_chunk_state() == ChunkState::kBeingWritten);
  PERFETTO_DCHECK(!has_open_fragment());

  // EndFragment publishes before another fragment can begin, so a rewrite
  // leaves exactly its just-ended fragment to relocate. FinishCurrentChunk
  // abandons the open fragment and leaves no completed suffix.
  //
  //   BeingWritten(N) -- writer --> Complete(N + has_suffix)
  //          |
  //          +--------- reader --> RewriteRequested(N)
  //                                      |
  //                         writer copies suffix and acknowledges
  //
  // A replacement has BeingWritten(0) and the same one-fragment suffix. It can
  // lose the same race, hence the loop and the unchanged suffix_size.
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
      //    and a scraped prefix could then end in the middle of a packet.
      //  - The fragment count is at its maximum.
      //  - At most one byte is left between the payload and the sizes.
      //    BeginFragment() hands out a non-empty range, and the size varint
      //    needs one more byte, so a one-byte gap cannot be reused even by a
      //    zero-length fragment; two bytes can. EndFragment() only accepts a
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
          cur_chunk_idx_, writer_id_, expected);
    }

    const uint32_t taken = NumFragmentsOf(expected);
    PERFETTO_DCHECK(taken == NumFragmentsOf(expected_state_word_));
    PERFETTO_DCHECK(num_fragments_ ==
                    taken + (suffix_size.has_value() ? 1u : 0u));

    // Save the suffix before acknowledging: the reader can then reclaim the
    // old chunk. Acknowledge before waiting for space to avoid pinning it.
    if (suffix_size) {
      // TODO(sashwinbalaji): Measure first-relocation latency and retained
      // memory per writer before deciding whether to preallocate this scratch
      // or release it after use.
      relocation_payload_.assign(cur_chunk_ + payload_end_ - *suffix_size,
                                 cur_chunk_ + payload_end_);
    }

    if (!ring_->TryAcknowledgeRewrite(cur_chunk_idx_, expected)) {
      PERFETTO_FATAL(
          "tracing v2: writer %u could not acknowledge chunk %u; only its "
          "owner may leave RewriteRequested",
          writer_id_, cur_chunk_idx_);
    }
    ++stats_.relocations;

    // Prefix flags move only when the reader took no fragments.
    const uint32_t inherited_flags =
        PayloadFlagsOf(expected) &
        (kFlagContinuesFromPrevChunk | kFlagDataLoss);
    const uint32_t relocated_flags = taken == 0 ? inherited_flags : 0;

    ResetCurrentChunk();

    if (!suffix_size) {
      // Nothing remains to relocate. A loss flag the reader did not take has
      // no suffix to travel with, so the next chunk must carry it instead.
      PERFETTO_DCHECK(!continues_on_next);
      if (relocated_flags & kFlagDataLoss)
        data_loss_pending_ = true;
      return EndFragmentResult::kSuccess;
    }

    if (AcquireNewChunk(relocated_flags) != BeginFragmentResult::kSuccess) {
      PERFETTO_DLOG(
          "tracing v2: writer %u dropped one relocated fragment; no "
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
    payload_end_ = kTargetBufferPayloadOffset + *suffix_size;
    uint8_t* sizes_begin =
        WriteFragmentSize(cur_chunk_ + chunk_size_, *suffix_size);
    sizes_begin_ = static_cast<uint32_t>(sizes_begin - cur_chunk_);
    num_fragments_ = 1;
    // Round again to publish the replacement, which the reader may also scrape.
  }
}

void SharedRingBufferWriter::ResetCurrentChunk() {
  cur_chunk_ = nullptr;
  cur_chunk_idx_ = 0;
  expected_state_word_ = 0;
  payload_end_ = 0;
  sizes_begin_ = 0;
  num_fragments_ = 0;
  cur_fragment_begin_ = kNoFragmentOpen;
}

}  // namespace perfetto::tracing_v2
