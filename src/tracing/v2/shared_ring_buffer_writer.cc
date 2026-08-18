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
#include "src/tracing/v2/tracing_v2_abi.h"

namespace perfetto::tracing_v2 {

SharedRingBufferWriter::Delegate::~Delegate() = default;

SharedRingBufferWriter::SharedRingBufferWriter(
    SharedRingBuffer* ring,
    WriterID writer_id,
    BufferID target_buffer,
    BufferExhaustedPolicy buffer_exhausted_policy,
    Delegate* delegate)
    : ring_(ring),
      writer_id_(writer_id),
      target_buffer_(target_buffer),
      buffer_exhausted_policy_(buffer_exhausted_policy),
      delegate_(delegate),
      chunk_size_(ring->chunk_size()) {
  PERFETTO_CHECK(delegate_);
  relocation_payload_.reserve(MaxFragmentSizeForEmptyChunk(chunk_size_));
  relocation_fragment_sizes_.reserve(kMaxFragmentsPerChunk);
}

SharedRingBufferWriter::~SharedRingBufferWriter() {
  FinishCurrentChunk();
}

SharedRingBufferWriter::FragmentSpan SharedRingBufferWriter::OpenFragment(
    uint32_t min_size,
    bool continues_from_prev) {
  PERFETTO_DCHECK(!has_open_fragment());

  const uint32_t largest_possible_fragment =
      MaxFragmentSizeForEmptyChunk(chunk_size_);
  if (min_size > largest_possible_fragment)
    return FragmentSpan{Outcome::kTooLarge, nullptr, nullptr};

  if (current_chunk_) {
    PERFETTO_DCHECK(ChunkStateOf(expected_state_word_) ==
                    ChunkState::kComplete);
    if (MaxFragmentSizeInCurrentChunk() >= min_size &&
        ring_->TryReacquireChunkForWriting(current_chunk_index_,
                                           expected_state_word_)) {
      expected_state_word_ =
          ReplaceChunkState(expected_state_word_, ChunkState::kBeingWritten);
      return OpenFragmentInCurrentChunk();
    }
    // The chunk is full or the reader reclaimed it.
    ResetCurrentChunk();
  }

  const uint32_t carried_flags =
      continues_from_prev ? uint32_t{kFlagContinuesFromPrevChunk} : 0u;
  const Outcome outcome = AcquireNewChunk(carried_flags);
  if (outcome != Outcome::kOk)
    return FragmentSpan{outcome, nullptr, nullptr};
  return OpenFragmentInCurrentChunk();
}

SharedRingBufferWriter::Outcome SharedRingBufferWriter::CloseFragment(
    uint32_t size,
    bool continues_on_next) {
  PERFETTO_DCHECK(has_open_fragment());
  PERFETTO_DCHECK(current_chunk_);
  PERFETTO_DCHECK(ChunkStateOf(expected_state_word_) ==
                  ChunkState::kBeingWritten);
  PERFETTO_DCHECK(size <= open_fragment_end_ - open_fragment_begin_);

  // Nothing becomes visible until CompleteCurrentChunk().
  const uint32_t varint_bytes = FragmentSizeVarIntBytes(size);
  PERFETTO_DCHECK(open_fragment_begin_ + size <= fragment_sizes_begin_);
  PERFETTO_DCHECK(varint_bytes <=
                  fragment_sizes_begin_ - (open_fragment_begin_ + size));
  fragment_sizes_begin_ = static_cast<uint32_t>(
      WriteFragmentSize(current_chunk_ + fragment_sizes_begin_, size) -
      current_chunk_);
  payload_end_ = open_fragment_begin_ + size;
  ++num_fragments_;
  open_fragment_begin_ = kNoOpenFragment;

  return CompleteCurrentChunk(continues_on_next);
}

SharedRingBufferWriter::Outcome SharedRingBufferWriter::FinishCurrentChunk() {
  // An open fragment is not counted and can be abandoned.
  open_fragment_begin_ = kNoOpenFragment;

  if (current_chunk_ &&
      ChunkStateOf(expected_state_word_) == ChunkState::kBeingWritten)
    return CompleteCurrentChunk(/*continues_on_next=*/false);

  ResetCurrentChunk();
  return Outcome::kOk;
}

uint32_t SharedRingBufferWriter::MaxFragmentSizeInCurrentChunk() const {
  if (!current_chunk_)
    return 0;
  if (num_fragments_ >= kMaxFragmentsPerChunk)
    return 0;
  if (fragment_sizes_begin_ <= payload_end_)
    return 0;
  const uint32_t available_bytes = fragment_sizes_begin_ - payload_end_;
  return MaxFragmentSizeForAvailableBytes(available_bytes);
}

SharedRingBufferWriter::Outcome SharedRingBufferWriter::AcquireNewChunk(
    uint32_t carried_flags) {
  PERFETTO_DCHECK(!current_chunk_);

  BufferExhaustedPolicy policy = buffer_exhausted_policy_;
  // Once data has been dropped, keep trying with kDrop until a chunk is
  // acquired. That chunk reports the gap and lets the next full-ring event
  // stall again.
  if (policy == BufferExhaustedPolicy::kStallThenDrop && data_loss_pending_)
    policy = BufferExhaustedPolicy::kDrop;

  // BufferExhaustedPolicy gives a stalling writer a few seconds before kStall
  // aborts or kStallThenDrop starts dropping. Use the same 30 second ceiling as
  // the v1 arbiter. A deadline, rather than a retry count, also bounds spurious
  // futex wakes.
  constexpr uint32_t kStallTimeoutMs = 30000;
  std::optional<base::TimeMillis> stall_deadline;

  uint32_t flags = carried_flags;
  if (data_loss_pending_)
    flags |= kFlagDataLoss;

  uint32_t failed_claims = 0;
  bool saw_unclaimable_chunk = false;
  for (;;) {
    const auto reservation = ring_->TryReserveWritePos();

    if (reservation.result == SharedRingBuffer::ReserveResult::kReserved) {
      const uint32_t being_written_word =
          MakeDataStateWord(ChunkState::kBeingWritten,
                            ChunkFormat::kTargetBuffer, flags, 0, writer_id_);
      if (ring_->TryAcquireChunkForWriting(reservation.position,
                                           being_written_word)) {
        current_chunk_index_ =
            ChunkIndexOfPosition(reservation.position, ring_->num_chunks());
        current_chunk_ = ring_->chunk_at(current_chunk_index_);
        expected_state_word_ = being_written_word;
        payload_end_ = kTargetBufferPayloadOffset;
        fragment_sizes_begin_ = chunk_size_;
        num_fragments_ = 0;
        data_loss_pending_ = false;
        StoreTargetBufferId(current_chunk_, target_buffer_);
        return Outcome::kOk;
      }

      // This reservation is now a hole. Never retry it against a different
      // Free word; reserve a later position instead.
      ++num_failed_claims_;
      saw_unclaimable_chunk = true;
      if (++failed_claims < ring_->num_chunks())
        continue;
    }

    // A stalling writer cannot make progress until the reader runs. A dropping
    // writer only needs to notify it after creating holes.
    if (failed_claims != 0 || policy != BufferExhaustedPolicy::kDrop) {
      delegate_->NotifyReader();
      failed_claims = 0;
    }

    const Outcome exhausted_outcome =
        saw_unclaimable_chunk ? Outcome::kNoChunkAvailable : Outcome::kFull;
    if (policy == BufferExhaustedPolicy::kDrop)
      return exhausted_outcome;

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
      return exhausted_outcome;
    }

    const uint32_t timeout_ms =
        static_cast<uint32_t>((*stall_deadline - now).count());
    const SharedRingBuffer::WriterWaitResult wait =
        ring_->WaitForReadPosChange(reservation.read_pos_sample, timeout_ms);
    if (wait == SharedRingBuffer::WriterWaitResult::kUnavailable) {
      if (policy == BufferExhaustedPolicy::kStall) {
        PERFETTO_FATAL(
            "tracing v2: writer %u cannot stall because waiting on read_pos "
            "is unavailable",
            writer_id_);
      }
      return exhausted_outcome;
    }
  }
}

SharedRingBufferWriter::Outcome SharedRingBufferWriter::CompleteCurrentChunk(
    bool continues_on_next) {
  PERFETTO_DCHECK(current_chunk_);
  PERFETTO_DCHECK(ChunkStateOf(expected_state_word_) ==
                  ChunkState::kBeingWritten);
  PERFETTO_DCHECK(!has_open_fragment());

  // expected_state_word_ contains the number of fragments visible before the
  // writer took the chunk. num_fragments_ also includes what it appended
  // afterwards. Usually the writer changes BeingWritten directly to Complete.
  // If the reader gets there first, it changes BeingWritten(N) to
  // RewriteRequested(N) and takes those N fragments. The writer then moves only
  // fragments N..M:
  //
  //   BeingWritten(N) -- writer --> Complete(M)
  //          |
  //          +--------- reader --> RewriteRequested(N)
  //                                      |
  //                         writer copies N..M and acknowledges
  //
  // A replacement can lose the same race, hence the loop.
  for (;;) {
    uint32_t flags = PayloadFlagsOf(expected_state_word_);
    if (continues_on_next)
      flags |= kFlagContinuesOnNextChunk;
    const uint32_t complete_word =
        MakeDataStateWord(ChunkState::kComplete, ChunkFormat::kTargetBuffer,
                          flags, num_fragments_, writer_id_);

    uint32_t observed = expected_state_word_;
    if (ring_->TrySetChunkComplete(current_chunk_index_, &observed,
                                   complete_word)) {
      expected_state_word_ = complete_word;
      // A Complete chunk carrying "continues on next chunk" is never reused.
      // Together with the rule that a BeingWritten word never carries that
      // flag, this guarantees that every non-empty prefix the reader can
      // scrape ends on a packet boundary.
      if (continues_on_next || MaxFragmentSizeInCurrentChunk() == 0)
        ResetCurrentChunk();
      return Outcome::kOk;
    }

    // Only the reader's rewrite request may beat this publication.
    if (ChunkStateOf(observed) != ChunkState::kRewriteRequested ||
        WriterIdOf(observed) != writer_id_) {
      PERFETTO_FATAL(
          "tracing v2: publication of chunk %u by writer %u lost to state word "
          "0x%08x, which is not a rewrite request for this writer",
          current_chunk_index_, writer_id_, observed);
    }

    // The reader took this prefix. The remaining fragments must move.
    const uint32_t taken = NumFragmentsOf(observed);
    PERFETTO_DCHECK(taken == NumFragmentsOf(expected_state_word_));
    PERFETTO_DCHECK(taken <= num_fragments_);
    const uint32_t suffix_fragments = num_fragments_ - taken;

    // Copy the suffix out before acknowledging the rewrite. After that, the
    // reader may reclaim the chunk.
    uint32_t suffix_begin = kTargetBufferPayloadOffset;
    relocation_fragment_sizes_.clear();
    const uint8_t* size_cursor = current_chunk_ + chunk_size_;
    for (uint32_t i = 0; i < num_fragments_; ++i) {
      uint32_t fragment_size = 0;
      if (!ReadFragmentSize(current_chunk_ + fragment_sizes_begin_,
                            &size_cursor, &fragment_size)) {
        PERFETTO_FATAL("tracing v2: writer %u corrupted its size directory",
                       writer_id_);
      }
      if (i < taken) {
        suffix_begin += fragment_size;
      } else {
        relocation_fragment_sizes_.push_back(fragment_size);
      }
    }
    PERFETTO_DCHECK(size_cursor == current_chunk_ + fragment_sizes_begin_);
    relocation_payload_.assign(current_chunk_ + suffix_begin,
                               current_chunk_ + payload_end_);

    // Acknowledge before looking for replacement capacity. The other order
    // leaves the old chunk occupied whenever the ring is full, so every later
    // traversal of it burns a position. Acknowledge even if the suffix ends up
    // dropped: the acknowledgement is about the chunk, not about whether the
    // data survived.
    if (!ring_->TryAcknowledgeRewrite(current_chunk_index_, observed)) {
      PERFETTO_FATAL(
          "tracing v2: writer %u could not acknowledge chunk %u; only its "
          "owner may leave RewriteRequested",
          writer_id_, current_chunk_index_);
    }
    ++num_relocations_;

    // Prefix flags move only when the reader took no fragments.
    const uint32_t relocated_flags =
        taken == 0 ? PayloadFlagsOf(observed) &
                         (kFlagContinuesFromPrevChunk | kFlagDataLoss)
                   : 0;

    ResetCurrentChunk();

    if (suffix_fragments == 0) {
      // Nothing remains to relocate.
      PERFETTO_DCHECK(!continues_on_next);
      return Outcome::kOk;
    }

    if (AcquireNewChunk(relocated_flags) != Outcome::kOk) {
      num_fragments_dropped_ += suffix_fragments;
      data_loss_pending_ = true;
      return Outcome::kRelocationDropped;
    }

    // Rebuild the suffix in the replacement. A suffix always fits: it came out
    // of a chunk of the same size.
    memcpy(current_chunk_ + kTargetBufferPayloadOffset,
           relocation_payload_.data(), relocation_payload_.size());
    payload_end_ = kTargetBufferPayloadOffset +
                   static_cast<uint32_t>(relocation_payload_.size());
    for (uint32_t fragment_size : relocation_fragment_sizes_) {
      fragment_sizes_begin_ = static_cast<uint32_t>(
          WriteFragmentSize(current_chunk_ + fragment_sizes_begin_,
                            fragment_size) -
          current_chunk_);
    }
    num_fragments_ = static_cast<uint32_t>(relocation_fragment_sizes_.size());
    // Round again to publish the replacement, which the reader may also scrape.
  }
}

void SharedRingBufferWriter::ResetCurrentChunk() {
  current_chunk_ = nullptr;
  current_chunk_index_ = 0;
  expected_state_word_ = 0;
  payload_end_ = 0;
  fragment_sizes_begin_ = 0;
  num_fragments_ = 0;
  open_fragment_begin_ = kNoOpenFragment;
  open_fragment_end_ = 0;
}

SharedRingBufferWriter::FragmentSpan
SharedRingBufferWriter::OpenFragmentInCurrentChunk() {
  PERFETTO_DCHECK(current_chunk_);
  PERFETTO_DCHECK(ChunkStateOf(expected_state_word_) ==
                  ChunkState::kBeingWritten);
  const uint32_t available = MaxFragmentSizeInCurrentChunk();
  PERFETTO_DCHECK(available > 0);
  open_fragment_begin_ = payload_end_;
  open_fragment_end_ = payload_end_ + available;
  return FragmentSpan{Outcome::kOk, current_chunk_ + open_fragment_begin_,
                      current_chunk_ + open_fragment_end_};
}

}  // namespace perfetto::tracing_v2
