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

#include <algorithm>
#include <optional>

#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"

namespace perfetto::tracing_v2 {
namespace {

// The 100 ms sleep limit matches SharedMemoryArbiterImpl::GetNewChunk().
constexpr uint32_t kMaxFallbackSleepUs = 100000;

}  // namespace

SharedRingBufferWriter::SharedRingBufferWriter(SharedRingBuffer* ring,
                                               WriterID writer_id,
                                               BufferID target_buffer)
    : ring_(ring),
      writer_id_(writer_id),
      target_buffer_(target_buffer),
      chunk_size_(ring->chunk_size()),
      max_fragment_size_(MaxFragmentSizeForEmptyChunk(chunk_size_)) {
  // The WriterID goes into every chunk that this writer claims. The caller
  // controls its lifetime.
  //
  // TraceWriterV2Impl releases it after its final publication. The service
  // can still read chunks under the ID after that. See the TODO in
  // ProducerRingBufferArbiter::OnWriterDestroyed().
  PERFETTO_DCHECK(writer_id_ != 0 && writer_id_ <= kMaxWriterID);
}

SharedRingBufferWriter::~SharedRingBufferWriter() {
  FinishCurrentChunk();
}

SharedRingBufferWriter::FragmentRange SharedRingBufferWriter::BeginFragment(
    uint32_t min_size,
    bool continues_from_prev) {
  PERFETTO_DCHECK(!has_open_fragment());
  // A new chunk now would put the next packet ahead of the saved fragment.
  PERFETTO_DCHECK(!pending_relocation_);
  // EndFragment(..., /*continues_on_next=*/true) discards the cached handle
  // when a packet continues into another chunk. A continuation must claim a
  // new chunk because only AcquireNewChunk() sets kFlagContinuesFromPrevChunk.
  PERFETTO_DCHECK(!continues_from_prev || !cur_chunk_);

  if (min_size > max_fragment_size_)
    return FragmentRange{BeginFragmentResult::kTooLarge};

  // Try the cached Complete chunk before a new reservation.
  if (cur_chunk_) {
    PERFETTO_DCHECK(cur_chunk_state() == ChunkState::kComplete);
    const uint32_t available = MaxFragmentSizeInCurrentChunk();
    // Pending loss does not prevent reuse, which requires no new reservation.
    // The next publication sets kFlagDataLoss. The reader then discards all
    // published fragments in this chunk, including those before the loss.
    if (available >= min_size && ring_->TryReacquireChunkForWriting(
                                     cur_chunk_idx_, expected_state_word_)) {
      expected_state_word_ =
          ReplaceChunkState(expected_state_word_, ChunkState::kBeingWritten);
      return BeginFragmentInCurrentChunk(available);
    }
    // The chunk has insufficient space or the reader reclaimed it first.
    // Discard the handle before the next claim.
    ResetCurrentChunk();
  }

  const uint32_t flags = continues_from_prev ? kFlagContinuesFromPrevChunk : 0u;
  const BeginFragmentResult result = AcquireNewChunk(flags);
  if (result != BeginFragmentResult::kSuccess)
    return FragmentRange{result};
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
  PERFETTO_DCHECK(size_directory_bytes_ <= chunk_size_);
  PERFETTO_DCHECK(cur_fragment_begin_ <= chunk_size_ - size_directory_bytes_);
  // The fragment must leave space for its size entry before the directory.
  PERFETTO_DCHECK(
      size <= MaxFragmentSizeForAvailableBytes(
                  chunk_size_ - size_directory_bytes_ - cur_fragment_begin_));

  // The reader cannot access this fragment until publication updates the shared
  // fragment count. Write the size entry before that publication.
  uint8_t* const chunk_end = cur_chunk_ + chunk_size_;
  uint8_t* const sizes_begin =
      WriteFragmentSizeReversed(chunk_end - size_directory_bytes_, size);
  // sizes_begin points to the new size entry, before the existing entries.
  // Subtract it from chunk_end to count all directory bytes, including the new
  // entry. The directory grows towards lower addresses.
  size_directory_bytes_ = static_cast<uint32_t>(chunk_end - sizes_begin);
  payload_end_ = cur_fragment_begin_ + size;
  ++num_fragments_;
  cur_fragment_begin_ = kNoFragmentOpen;

  return ReleaseCurrentChunkAsComplete(size, continues_on_next);
}

SharedRingBufferWriter::EndFragmentResult
SharedRingBufferWriter::FinishCurrentChunk() {
  PERFETTO_DCHECK(!pending_relocation_);
  // Abandon any open fragment. Its bytes are outside the published count, so
  // this does not affect the fragments the reader can already access.
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
  PERFETTO_DCHECK(payload_end_ <= chunk_size_);
  PERFETTO_DCHECK(size_directory_bytes_ <= chunk_size_ - payload_end_);
  const uint32_t available_bytes =
      chunk_size_ - payload_end_ - size_directory_bytes_;
  return MaxFragmentSizeForAvailableBytes(available_bytes);
}

SharedRingBufferWriter::BeginFragmentResult
SharedRingBufferWriter::AcquireNewChunk(uint32_t continuation_flags) {
  PERFETTO_DCHECK(!cur_chunk_);

  // A new chunk must preserve loss from either source:
  // - data_loss_pending_: this writer has a loss to report.
  // - continuation_flags: a rewrite of BeingWritten(0) left a loss unreported.
  //   The replacement inherits that flag even if data_loss_pending_ is false.
  const uint32_t flags =
      continuation_flags | (data_loss_pending_ ? kFlagDataLoss : 0u);
  const uint32_t being_written_word =
      MakeDataStateWord(ChunkState::kBeingWritten, ChunkFormat::kTargetBuffer,
                        flags, 0, writer_id_);
  const uint32_t num_chunks = ring_->num_chunks();

  // After num_chunks failed claims, return without a chunk.
  //
  // Other writers can reserve intervening positions, so our attempts
  // can select the same physical chunk on different traversals.
  // This limit bounds attempts, but does not guarantee a visit to every chunk.
  uint32_t num_failed_claims = 0;

  // Each attempt reserves a position, then tries to claim its physical chunk:
  //
  // 1. TryReserveWritePos() finds the ring buffer full.
  //    - write_pos stays unchanged. No position was reserved.
  //    - Return without a chunk below.
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
  //    - After num_chunks failed claims, return without a chunk instead.
  for (;;) {
    const auto reservation = ring_->TryReserveWritePos();
    read_pos_for_wait_ = reservation.read_pos_for_wait;

    if (reservation.result == SharedRingBuffer::ReserveResult::kReserved) {
      // Claim the chunk only if its Free word matches this reservation's wrap.
      if (ring_->TryAcquireChunkForWriting(reservation.chunk_pos,
                                           being_written_word)) {
        cur_chunk_idx_ =
            ChunkIndex::FromPosition(reservation.chunk_pos, num_chunks);
        cur_chunk_ = ring_->chunk_at(cur_chunk_idx_);
        expected_state_word_ = being_written_word;
        payload_end_ = kTargetBufferPayloadOffset;
        size_directory_bytes_ = 0;
        num_fragments_ = 0;
        // The shared word now contains any pending loss. If a rewrite prevents
        // publication, relocation preserves the flag from that word.
        data_loss_pending_ = false;
        fallback_sleep_us_ = 0;
        StoreTargetBufferId(cur_chunk_, target_buffer_);
        return BeginFragmentResult::kSuccess;
      }

      ++stats_.failed_claims;
      // write_pos advanced, but the writer did not acquire the physical chunk.
      // This leaves an unclaimed position for the reader to consume, unless it
      // already consumed the position before our claim.
      //
      // Retry with a new reservation, which supplies both the index and wrap
      // count. Other writers can reserve intervening positions, so this need
      // not select the next physical chunk. A different Free wrap belongs to
      // a position we did not reserve. Do not use it to retry the failed claim.
      if (++num_failed_claims < num_chunks)
        continue;
    }

    // If we reach this point, either:
    // - TryReserveWritePos() found the ring buffer full and did not increment
    //   write_pos.
    // - TryAcquireChunkForWriting() failed num_chunks times in this round.
    //   Earlier failures took continue above to try another reservation.
    //   The last failure reached the attempt limit.

    // The result tells why the call found no chunk:
    // - kClaimFailed: a claim failed at least once in this call, even if a
    //   later reservation found the ring buffer full.
    // - kFull: every reservation found the ring buffer full.
    //
    // The caller applies its BufferExhaustedPolicy. To wait, it calls
    // WaitForReadPosChange() and then calls again.
    return num_failed_claims != 0 ? BeginFragmentResult::kClaimFailed
                                  : BeginFragmentResult::kFull;
  }
}

void SharedRingBufferWriter::WaitForReadPosChange(uint32_t timeout_ms) {
  if (use_futex_) {
    // Wait while read_pos equals the value from the last reservation attempt.
    const SharedRingBuffer::WriterWaitResult result =
        ring_->WaitForReadPosChange(read_pos_for_wait_, timeout_ms);
    if (result == SharedRingBuffer::WriterWaitResult::kRetry)
      return;
    // If the futex is unavailable, use fallback sleeps for this writer's later
    // waits. This avoids repeated calls to an unsupported syscall.
    use_futex_ = false;
  }

  // No futex. Sleep with v1's backoff: 0, 8, 72, ... microseconds, up to
  // kMaxFallbackSleepUs. No sleep requests more than |timeout_ms|.
  const uint64_t timeout_us = uint64_t{timeout_ms} * 1000;
  base::SleepMicroseconds(static_cast<unsigned>(
      std::min<uint64_t>(fallback_sleep_us_, timeout_us)));
  fallback_sleep_us_ =
      std::min(kMaxFallbackSleepUs, (fallback_sleep_us_ + 1) * 8);
}

SharedRingBufferWriter::EndFragmentResult
SharedRingBufferWriter::ReleaseCurrentChunkAsComplete(
    std::optional<uint32_t> fragment_size,
    bool continues_on_next) {
  PERFETTO_DCHECK(cur_chunk_);
  PERFETTO_DCHECK(cur_chunk_state() == ChunkState::kBeingWritten);
  PERFETTO_DCHECK(!has_open_fragment());

  // Publication competes with the reader's rewrite request. For N published
  // fragments, the possible transitions are:
  // - Writer: BeingWritten(N) -> Complete(num_fragments_).
  // - Reader: BeingWritten(N) -> RewriteRequested(N).
  //
  // On failure, relocate any unpublished fragment and retry publication.
  // The reader can also request a rewrite of the replacement chunk.
  // Repeat until publication succeeds or acquisition of a replacement fails.
  //
  // A failed acquisition leaves the relocation pending.
  for (;;) {
    uint32_t flags = PayloadFlagsOf(expected_state_word_);
    // Publish pending loss and the fragment count in the same state word.
    // The reader must observe the loss flag before it can deliver fragments.
    if (data_loss_pending_)
      flags |= kFlagDataLoss;
    if (continues_on_next)
      flags |= kFlagContinuesOnNextChunk;
    const uint32_t complete_word =
        MakeDataStateWord(ChunkState::kComplete, ChunkFormat::kTargetBuffer,
                          flags, num_fragments_, writer_id_);

    uint32_t expected = expected_state_word_;
    if (ring_->TryReleaseChunkAsComplete(cur_chunk_idx_, complete_word,
                                         &expected)) {
      expected_state_word_ = complete_word;
      data_loss_pending_ = false;
      PERFETTO_DCHECK(payload_end_ <= chunk_size_);
      PERFETTO_DCHECK(size_directory_bytes_ <= chunk_size_ - payload_end_);

      // Discard the cached handle if any of these limits prevents reuse:
      // - continues_on_next: kFlagContinuesOnNextChunk describes the last
      //   fragment. If another fragment follows, the flag describes the wrong
      //   fragment and the reader cannot identify the split packet.
      // - num_fragments_ reached kMaxFragmentsPerChunk.
      // - At most one byte remains between the payload and the size directory.
      //   BeginFragment() requires a non-empty range plus one size byte, even
      //   if the caller later ends the fragment with a size of zero.
      if (continues_on_next || num_fragments_ >= kMaxFragmentsPerChunk ||
          chunk_size_ - payload_end_ - size_directory_bytes_ <= 1) {
        ResetCurrentChunk();
      }
      return EndFragmentResult::kSuccess;
    }

    // On failure, expected must be RewriteRequested for this writer. Only the
    // reader can change the word before publication, so any other value is a
    // protocol error.
    if (PERFETTO_UNLIKELY(ChunkStateOf(expected) !=
                              ChunkState::kRewriteRequested ||
                          WriterIDOf(expected) != writer_id_)) {
      // traced keeps its own mapping, so it reads the record after the abort.
      ring_->RecordFirstWriterFailure(
          {WriterFailureReason::kPublicationLost, writer_id_, expected});
      PERFETTO_FATAL(
          "tracing v2: publication of chunk %u by writer %u lost to state word "
          "0x%08x, which is not a rewrite request for this writer",
          cur_chunk_idx_.value(), writer_id_, expected);
    }

    // expected is RewriteRequested: the reader did a partial chunk read while
    // this writer prepared another fragment.
    // - It consumed NumFragmentsOf(expected) published fragments. It discarded
    //   them if kFlagDataLoss was set or validation failed.
    // - It did not consume the unpublished fragment. The reader will not return
    //   to this reservation for more fragments.
    // - The writer must publish the remaining
    //   num_fragments_ - NumFragmentsOf(expected) fragments in a new chunk.
    const uint32_t num_fragments_read = NumFragmentsOf(expected);

    // The rewrite request preserves the count from our last publication.
    PERFETTO_DCHECK(num_fragments_read == NumFragmentsOf(expected_state_word_));

    // At most one fragment needs relocation because EndFragment() publishes
    // before another fragment can begin:
    // - EndFragment() adds the finished fragment to num_fragments_.
    // - FinishCurrentChunk() abandons the open fragment without increasing it.
    PERFETTO_DCHECK(num_fragments_ ==
                    num_fragments_read + (fragment_size.has_value() ? 1u : 0u));

    // The unpublished fragment occupies the last |*fragment_size| bytes before
    // payload_end_. The reader already consumed the earlier fragments.
    // Save these bytes before acknowledgement. The reader can then reclaim
    // the chunk.
    if (fragment_size) {
      // TODO(sashwinbalaji): Measure allocation latency on the first relocation
      // and retained memory per writer. Use the results to decide when to
      // allocate this scratch buffer and whether to retain it for later use.
      relocation_payload_.assign(cur_chunk_ + payload_end_ - *fragment_size,
                                 cur_chunk_ + payload_end_);
    }

    // Acknowledge before the next acquisition. The reader can then reclaim the
    // old chunk even if this writer must wait for replacement space.
    // On failure, |observed| holds the word that the CAS found.
    uint32_t observed = expected;
    if (PERFETTO_UNLIKELY(
            !ring_->TryAcknowledgeRewrite(cur_chunk_idx_, &observed))) {
      ring_->RecordFirstWriterFailure(
          {WriterFailureReason::kAcknowledgementFailed, writer_id_, observed});
      PERFETTO_FATAL(
          "tracing v2: writer %u could not acknowledge chunk %u: only its "
          "owner may leave RewriteRequested, but the word is 0x%08x",
          writer_id_, cur_chunk_idx_.value(), observed);
    }
    ++stats_.relocations;

    // If num_fragments_read is zero, preserve both flags:
    // - kFlagContinuesFromPrevChunk still describes the unpublished fragment.
    // - kFlagDataLoss still needs a report because the reader ignores loss in
    //   BeingWritten(0).
    //
    // Otherwise, clear both flags:
    // - The reader consumed the first fragment, so its continuation flag no
    //   longer applies. The unpublished fragment starts a new packet because
    //   EndFragment(..., /*continues_on_next=*/true) prevents further fragments
    //   in the old chunk.
    // - The reader handles the old loss. The delegate tracks that gap for the
    //   next complete packet. Repeating the flag would discard the replacement.
    //
    // kFlagContinuesOnNextChunk comes from this call's continues_on_next value.
    const uint32_t inherited_flags =
        PayloadFlagsOf(expected) &
        (kFlagContinuesFromPrevChunk | kFlagDataLoss);
    const uint32_t relocated_flags =
        num_fragments_read == 0 ? inherited_flags : 0;

    ResetCurrentChunk();

    if (!fragment_size) {
      PERFETTO_DCHECK(!continues_on_next);
      // FinishCurrentChunk() has no fragment to relocate. If the reader did not
      // report the old loss, the next acquired chunk must report it.
      if (relocated_flags & kFlagDataLoss)
        data_loss_pending_ = true;
      return EndFragmentResult::kSuccess;
    }

    // AcquireNewChunk() also includes data_loss_pending_. A loss recorded after
    // the old publication must survive even if the reader handled the old flag.
    const BeginFragmentResult claim = AcquireNewChunk(relocated_flags);
    if (claim != BeginFragmentResult::kSuccess) {
      // Keep the saved fragment. The caller retries or drops it.
      pending_relocation_ =
          PendingRelocation{relocated_flags, continues_on_next};
      return claim == BeginFragmentResult::kFull
                 ? EndFragmentResult::kFull
                 : EndFragmentResult::kClaimFailed;
    }
    if (PERFETTO_UNLIKELY(after_replacement_claim_for_testing_))
      after_replacement_claim_for_testing_();
    CopyRelocatedFragment(*fragment_size);
  }
}

SharedRingBufferWriter::RetryRelocationResult
SharedRingBufferWriter::RetryRelocation() {
  PERFETTO_DCHECK(pending_relocation_);
  PERFETTO_DCHECK(!cur_chunk_);

  RetryRelocationResult retry;
  const BeginFragmentResult claim =
      AcquireNewChunk(pending_relocation_->chunk_flags);
  if (claim != BeginFragmentResult::kSuccess) {
    retry.result = claim == BeginFragmentResult::kFull
                       ? EndFragmentResult::kFull
                       : EndFragmentResult::kClaimFailed;
    return retry;
  }
  retry.acquired_replacement = true;
  if (PERFETTO_UNLIKELY(after_replacement_claim_for_testing_))
    after_replacement_claim_for_testing_();

  const auto fragment_size = static_cast<uint32_t>(relocation_payload_.size());
  const bool continues_on_next = pending_relocation_->continues_on_next;
  pending_relocation_.reset();
  CopyRelocatedFragment(fragment_size);
  // If the reader also takes this replacement, this relocates again.
  retry.result =
      ReleaseCurrentChunkAsComplete(fragment_size, continues_on_next);
  return retry;
}

void SharedRingBufferWriter::DropRelocation() {
  PERFETTO_DCHECK(pending_relocation_);
  pending_relocation_.reset();
  ++stats_.fragments_dropped;
  // Also covers a loss flag that the relocation inherited.
  data_loss_pending_ = true;
}

void SharedRingBufferWriter::CopyRelocatedFragment(uint32_t fragment_size) {
  // All chunks have the same capacity, so the saved fragment and its size
  // entry fit in the empty replacement. An empty payload still needs a size
  // entry and contributes one to the fragment count.
  if (!relocation_payload_.empty()) {
    memcpy(cur_chunk_ + kTargetBufferPayloadOffset, relocation_payload_.data(),
           relocation_payload_.size());
  }
  payload_end_ = kTargetBufferPayloadOffset + fragment_size;
  uint8_t* const chunk_end = cur_chunk_ + chunk_size_;
  uint8_t* const sizes_begin =
      WriteFragmentSizeReversed(chunk_end, fragment_size);
  size_directory_bytes_ = static_cast<uint32_t>(chunk_end - sizes_begin);
  num_fragments_ = 1;
}

void SharedRingBufferWriter::ResetCurrentChunk() {
  cur_chunk_ = nullptr;
  cur_chunk_idx_ = ChunkIndex::FromIndex(0);
  expected_state_word_ = 0;
  payload_end_ = 0;
  size_directory_bytes_ = 0;
  num_fragments_ = 0;
  cur_fragment_begin_ = kNoFragmentOpen;
}

}  // namespace perfetto::tracing_v2
