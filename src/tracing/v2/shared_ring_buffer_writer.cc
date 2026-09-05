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

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"

namespace perfetto::tracing_v2 {

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
  // WriterIDs come from the v1 IdAllocator range: never zero and at most
  // kMaxWriterID.
  PERFETTO_DCHECK(writer_id_ != 0 && writer_id_ <= kMaxWriterID);
  relocation_payload_.reserve(max_fragment_size_);
  relocation_fragment_sizes_.reserve(kMaxFragmentsPerChunk);
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
    return FragmentRange{BeginFragmentResult::kTooLarge, nullptr, nullptr};

  // Common case: we take our own Complete chunk back. We don't do that after
  // a loss, though: the next fragment has to land in a chunk that reports the
  // gap, and reusing this one would put post-loss data ahead of the flag.
  if (cur_chunk_) {
    PERFETTO_DCHECK(ChunkStateOf(expected_state_word_) ==
                    ChunkState::kComplete);
    const uint32_t available = MaxFragmentSizeInCurrentChunk();
    if (!data_loss_pending_ && available >= min_size &&
        PERFETTO_LIKELY(ring_->TryReacquireChunkForWriting(
            cur_chunk_idx_, expected_state_word_))) {
      expected_state_word_ =
          ReplaceChunkState(expected_state_word_, ChunkState::kBeingWritten);
      return BeginFragmentInCurrentChunk(available);
    }
    // The chunk is full, a loss is pending, or the reader reclaimed it.
    ResetCurrentChunk();
  }

  const uint32_t flags =
      continues_from_prev ? static_cast<uint32_t>(kFlagContinuesFromPrevChunk)
                          : 0u;
  const BeginFragmentResult result = AcquireNewChunk(flags);
  if (result != BeginFragmentResult::kSuccess)
    return FragmentRange{result, nullptr, nullptr};
  // A fresh chunk offers everything after its header.
  return BeginFragmentInCurrentChunk(max_fragment_size_);
}

SharedRingBufferWriter::FragmentRange
SharedRingBufferWriter::BeginFragmentInCurrentChunk(uint32_t available) {
  PERFETTO_DCHECK(cur_chunk_);
  PERFETTO_DCHECK(ChunkStateOf(expected_state_word_) ==
                  ChunkState::kBeingWritten);
  PERFETTO_DCHECK(available > 0 &&
                  available == MaxFragmentSizeInCurrentChunk());
  cur_fragment_begin_ = payload_end_;
  return FragmentRange{BeginFragmentResult::kSuccess,
                       cur_chunk_ + cur_fragment_begin_,
                       cur_chunk_ + cur_fragment_begin_ + available};
}

SharedRingBufferWriter::EndFragmentResult SharedRingBufferWriter::EndFragment(
    uint32_t size,
    bool continues_on_next) {
  PERFETTO_DCHECK(has_open_fragment());
  PERFETTO_DCHECK(cur_chunk_);
  PERFETTO_DCHECK(ChunkStateOf(expected_state_word_) ==
                  ChunkState::kBeingWritten);
  // |size| must still fit in the range BeginFragment() handed out, together
  // with the size varint that encodes it.
  PERFETTO_DCHECK(size <= MaxFragmentSizeForAvailableBytes(
                              sizes_begin_ - cur_fragment_begin_));

  // Nothing becomes visible until CompleteCurrentChunk().
  uint8_t* sizes_begin = cur_chunk_ + sizes_begin_;
  sizes_begin = WriteFragmentSize(sizes_begin, size);
  sizes_begin_ = static_cast<uint32_t>(sizes_begin - cur_chunk_);
  payload_end_ = cur_fragment_begin_ + size;
  ++num_fragments_;
  cur_fragment_begin_ = kNoFragmentOpen;

  return CompleteCurrentChunk(continues_on_next);
}

SharedRingBufferWriter::EndFragmentResult
SharedRingBufferWriter::FinishCurrentChunk() {
  // An open fragment is not counted and can be abandoned.
  cur_fragment_begin_ = kNoFragmentOpen;

  if (cur_chunk_ &&
      ChunkStateOf(expected_state_word_) == ChunkState::kBeingWritten)
    return CompleteCurrentChunk(/*continues_on_next=*/false);

  ResetCurrentChunk();
  return EndFragmentResult::kSuccess;
}

uint32_t SharedRingBufferWriter::MaxFragmentSizeInCurrentChunk() const {
  if (!cur_chunk_)
    return 0;
  if (num_fragments_ >= kMaxFragmentsPerChunk)
    return 0;
  if (sizes_begin_ <= payload_end_)
    return 0;
  const uint32_t available_bytes = sizes_begin_ - payload_end_;
  return MaxFragmentSizeForAvailableBytes(available_bytes);
}

SharedRingBufferWriter::BeginFragmentResult
SharedRingBufferWriter::AcquireNewChunk(uint32_t flags) {
  PERFETTO_DCHECK(!cur_chunk_);

  // kStallThenDrop waits only on the first exhaustion. Once the packet writer
  // has recorded a loss, later probes use kDrop so that each dropped packet
  // does not stall for another timeout. The first chunk acquired afterwards
  // carries kFlagDataLoss and clears data_loss_pending_, which lets a later
  // exhaustion stall again. This matches v1's drop_packets_ behaviour.
  BufferExhaustedPolicy policy = buffer_exhausted_policy_;
  if (policy == BufferExhaustedPolicy::kStallThenDrop && data_loss_pending_)
    policy = BufferExhaustedPolicy::kDrop;

  // Match v1's approximately 30-second ceiling for kStall and kStallThenDrop.
  // Keep this in sync with SharedMemoryArbiterImpl::GetNewChunk().
  constexpr uint32_t kStallTimeoutMs = 30000;
  std::optional<base::TimeMillis> stall_deadline;

  if (data_loss_pending_)
    flags |= kFlagDataLoss;
  const uint32_t being_written_word =
      MakeDataStateWord(ChunkState::kBeingWritten, ChunkFormat::kTargetBuffer,
                        flags, 0, writer_id_);

  // Each round reserves a position and then claims its chunk.
  //  - A failed reservation means num_chunks positions are already
  //    outstanding. Nothing was reserved, so only the policy is left.
  //  - A reserved position whose claim succeeds returns the chunk.
  //  - A reserved position whose claim fails leaves a hole that only the
  //    reader can resolve. The writer tries later positions, but stops after
  //    one visit to every physical chunk. Past that point only the reader can
  //    make a chunk claimable again, and without the bound a writer racing a
  //    fast reader could keep punching holes while other writers pin every
  //    chunk.
  uint32_t num_failed_claims = 0;
  bool saw_unclaimable_chunk = false;
  for (;;) {
    const auto reservation = ring_->TryReserveWritePos();

    if (reservation.result == SharedRingBuffer::ReserveResult::kReserved) {
      // Happy case: the reserved position's chunk is Free for this traversal.
      if (PERFETTO_LIKELY(ring_->TryAcquireChunkForWriting(
              reservation.position, being_written_word))) {
        cur_chunk_idx_ =
            ChunkIndexOfPosition(reservation.position, ring_->num_chunks());
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
SharedRingBufferWriter::CompleteCurrentChunk(bool continues_on_next) {
  PERFETTO_DCHECK(cur_chunk_);
  PERFETTO_DCHECK(ChunkStateOf(expected_state_word_) ==
                  ChunkState::kBeingWritten);
  PERFETTO_DCHECK(!has_open_fragment());

  // expected_state_word_ holds the fragment count the reader could already see
  // when this writer took the chunk; num_fragments_ also counts what we
  // appended since. Usually we move BeingWritten straight to Complete. If the
  // reader gets there first, it changes BeingWritten(N) to RewriteRequested(N)
  // and takes those N fragments, and we move only fragments N..M into a
  // replacement chunk:
  //
  //   BeingWritten(N) -- writer --> Complete(M)
  //          |
  //          +--------- reader --> RewriteRequested(N)
  //                                      |
  //                         writer copies N..M and acknowledges
  //
  // The replacement can lose the same race, hence the loop.
  for (;;) {
    uint32_t flags = PayloadFlagsOf(expected_state_word_);
    if (continues_on_next)
      flags |= kFlagContinuesOnNextChunk;
    const uint32_t complete_word =
        MakeDataStateWord(ChunkState::kComplete, ChunkFormat::kTargetBuffer,
                          flags, num_fragments_, writer_id_);

    uint32_t observed = expected_state_word_;
    // Happy case: nobody touched the chunk while we owned it.
    if (PERFETTO_LIKELY(ring_->TryReleaseChunkAsComplete(
            cur_chunk_idx_, &observed, complete_word))) {
      expected_state_word_ = complete_word;
      // Let go of the chunk when the next fragment must start elsewhere:
      //  - continues_on_next: kFlagContinuesOnNextChunk describes the last
      //    fragment. Appending another would make it describe the wrong one,
      //    and a scraped prefix could then end in the middle of a packet.
      //  - No representable fragment fits in the remaining space.
      if (continues_on_next || MaxFragmentSizeInCurrentChunk() == 0)
        ResetCurrentChunk();
      return EndFragmentResult::kSuccess;
    }

    // Only the reader's rewrite request may beat this publication.
    if (ChunkStateOf(observed) != ChunkState::kRewriteRequested ||
        WriterIDOf(observed) != writer_id_) {
      PERFETTO_FATAL(
          "tracing v2: publication of chunk %u by writer %u lost to state word "
          "0x%08x, which is not a rewrite request for this writer",
          cur_chunk_idx_, writer_id_, observed);
    }

    // The reader took this prefix. The remaining fragments must move.
    const uint32_t taken = NumFragmentsOf(observed);
    // The rewrite request passes the fragment count through untouched, so it
    // must still equal the prefix this writer published. That prefix cannot
    // exceed the fragments finalized so far. The suffix arithmetic below
    // relies on both.
    PERFETTO_DCHECK(taken == NumFragmentsOf(expected_state_word_));
    PERFETTO_DCHECK(taken <= num_fragments_);
    const uint32_t suffix_fragments = num_fragments_ - taken;

    // Copy the suffix before acknowledging: once acknowledged, the reader may
    // reuse the old chunk. Acknowledge before looking for replacement space so
    // a full ring does not leave the old chunk pinned.
    uint32_t suffix_begin = kTargetBufferPayloadOffset;
    relocation_fragment_sizes_.clear();
    const uint8_t* sizes_cursor = cur_chunk_ + chunk_size_;
    for (uint32_t i = 0; i < num_fragments_; ++i) {
      uint32_t fragment_size = 0;
      if (!ReadFragmentSize(cur_chunk_ + sizes_begin_, &sizes_cursor,
                            &fragment_size)) {
        PERFETTO_FATAL("tracing v2: writer %u corrupted its fragment sizes",
                       writer_id_);
      }
      if (i < taken) {
        suffix_begin += fragment_size;
      } else {
        relocation_fragment_sizes_.push_back(fragment_size);
      }
    }
    PERFETTO_DCHECK(sizes_cursor == cur_chunk_ + sizes_begin_);
    relocation_payload_.assign(cur_chunk_ + suffix_begin,
                               cur_chunk_ + payload_end_);

    if (!ring_->TryAcknowledgeRewrite(cur_chunk_idx_, observed)) {
      PERFETTO_FATAL(
          "tracing v2: writer %u could not acknowledge chunk %u; only its "
          "owner may leave RewriteRequested",
          writer_id_, cur_chunk_idx_);
    }
    ++stats_.relocations;

    // Prefix flags move only when the reader took no fragments.
    const uint32_t relocated_flags =
        taken == 0 ? PayloadFlagsOf(observed) &
                         (kFlagContinuesFromPrevChunk | kFlagDataLoss)
                   : 0;

    ResetCurrentChunk();

    if (suffix_fragments == 0) {
      // Nothing remains to relocate. A loss flag the reader did not take has
      // no suffix to travel with, so the next chunk must carry it instead.
      PERFETTO_DCHECK(!continues_on_next);
      if (relocated_flags & kFlagDataLoss)
        data_loss_pending_ = true;
      return EndFragmentResult::kSuccess;
    }

    if (AcquireNewChunk(relocated_flags) != BeginFragmentResult::kSuccess) {
      PERFETTO_DLOG(
          "tracing v2: writer %u dropped %u relocated fragments; no "
          "replacement chunk",
          writer_id_, suffix_fragments);
      stats_.fragments_dropped += suffix_fragments;
      data_loss_pending_ = true;
      return EndFragmentResult::kRelocationDropped;
    }

    // Rebuild the suffix in the replacement. A suffix always fits: it came out
    // of a chunk of the same size.
    memcpy(cur_chunk_ + kTargetBufferPayloadOffset, relocation_payload_.data(),
           relocation_payload_.size());
    payload_end_ = kTargetBufferPayloadOffset +
                   static_cast<uint32_t>(relocation_payload_.size());
    uint8_t* sizes_begin = cur_chunk_ + chunk_size_;
    for (uint32_t fragment_size : relocation_fragment_sizes_)
      sizes_begin = WriteFragmentSize(sizes_begin, fragment_size);
    sizes_begin_ = static_cast<uint32_t>(sizes_begin - cur_chunk_);
    num_fragments_ = static_cast<uint32_t>(relocation_fragment_sizes_.size());
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
