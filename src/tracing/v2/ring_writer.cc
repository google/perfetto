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

#include "src/tracing/v2/ring_writer.h"

#include <stdint.h>
#include <string.h>

#include "perfetto/base/logging.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/tracing_v2_abi.h"

namespace perfetto::tracing_v2 {
namespace {

// A reserved position gets exactly one claim attempt, and a failed claim costs
// the ring a position. Spinning through them is worse than telling the caller
// no, so the loop gives up after a handful.
constexpr uint32_t kMaxBurnedClaims = 4;

// Mirrors the v1 arbiter's stall shape (see kAssertAtNStalls and
// kMaxStallIntervalUs in shared_memory_arbiter_impl.cc) so a stalled v2 writer
// behaves recognisably: bounded slices, then abort under kStall and drop under
// kStallThenDrop.
constexpr uint32_t kStallSliceMs = 100;
constexpr uint32_t kMaxStallSlices = 300;

// A relocation only repeats if the reader scrapes the replacement chunk too,
// and it can only do that once per position, so in practice this terminates
// immediately. The bound is here so that a reader keeping perfect pace with one
// writer cannot turn the publish path into an unbounded loop.
constexpr uint32_t kMaxRelocationAttempts = 8;

}  // namespace

RingWriter::RingWriter(SharedRingBuffer* ring,
                       WriterID writer_id,
                       BufferID target_buffer,
                       BufferExhaustedPolicy buffer_exhausted_policy)
    : ring_(ring),
      writer_id_(writer_id),
      target_buffer_(target_buffer),
      buffer_exhausted_policy_(buffer_exhausted_policy),
      chunk_size_(ring->chunk_size()),
      fragment_size_width_(ring->fragment_size_width()) {
  // Sized once, here, so that nothing on the relocation path allocates.
  relocation_payload_.reserve(chunk_size_);
  relocation_sizes_.reserve(kMaxFragmentsPerChunk);
}

RingWriter::~RingWriter() {
  Release();
}

uint32_t RingWriter::AvailableForNextFragment() const {
  if (ownership_ == Ownership::kNone)
    return 0;
  if (num_fragments_ >= kMaxFragmentsPerChunk)
    return 0;
  // The bytes the next fragment's size entry will occupy are set aside now, so
  // the reservation is arithmetic rather than a hole in the chunk.
  const uint32_t directory_bytes = (num_fragments_ + 1) * fragment_size_width_;
  if (directory_bytes >= chunk_size_)
    return 0;
  const uint32_t directory_start = chunk_size_ - directory_bytes;
  if (directory_start <= payload_end_)
    return 0;
  return directory_start - payload_end_;
}

RingWriter::FragmentSpan RingWriter::OpenFragment(uint32_t min_size,
                                                  bool continues_from_prev) {
  PERFETTO_DCHECK(!has_open_fragment());
  // Outside an open fragment this writer never holds an Acquired chunk:
  // CloseFragment() publishes and then either caches the Complete chunk or lets
  // it go. Keeping that invariant is what lets this function decide entirely
  // from |ownership_|.
  PERFETTO_DCHECK(ownership_ != Ownership::kAcquired);

  const uint32_t largest_possible_fragment =
      chunk_size_ - kTargetBufferPayloadOffset - fragment_size_width_;
  if (min_size > largest_possible_fragment)
    return FragmentSpan{Outcome::kTooLarge, nullptr, nullptr};

  if (ownership_ == Ownership::kCompleteCached) {
    // TryReuse() is only attempted when the chunk could actually take the
    // fragment; the short-circuit is what keeps a doomed reuse compare-and-swap
    // off the path.
    if (AvailableForNextFragment() >= min_size &&
        ring_->TryReuse(chunk_index_, state_word_)) {
      state_word_ = WithState(state_word_, ChunkState::kAcquired);
      ownership_ = Ownership::kAcquired;
      return OpenInHeldChunk();
    }
    // Either the chunk is too full to be worth taking back, or the reader
    // reclaimed it while we were away. Either way we are done with it: it is
    // already Complete, so the reader consumes it without our help.
    DropChunkHandle();
  }

  const uint32_t carried_flags =
      continues_from_prev ? kFlagContinuesFromPrevChunk : 0;
  const Outcome outcome = AcquireChunk(carried_flags);
  if (outcome != Outcome::kOk)
    return FragmentSpan{outcome, nullptr, nullptr};
  return OpenInHeldChunk();
}

RingWriter::FragmentSpan RingWriter::OpenInHeldChunk() {
  PERFETTO_DCHECK(ownership_ == Ownership::kAcquired);
  const uint32_t available = AvailableForNextFragment();
  PERFETTO_DCHECK(available > 0);
  open_fragment_begin_ = payload_end_;
  open_fragment_end_ = payload_end_ + available;
  return FragmentSpan{Outcome::kOk, chunk_ + open_fragment_begin_,
                      chunk_ + open_fragment_end_};
}

RingWriter::Outcome RingWriter::CloseFragment(uint32_t size,
                                              bool continues_on_next) {
  PERFETTO_DCHECK(has_open_fragment());
  PERFETTO_DCHECK(ownership_ == Ownership::kAcquired);
  PERFETTO_DCHECK(size <= open_fragment_end_ - open_fragment_begin_);

  // Write the actual size into the entry that was set aside, then move both
  // cursors and the local count. Nothing is visible to the reader
  // until the release publication below.
  StoreFragmentSize(
      chunk_ + FragmentSizeEntryOffset(chunk_size_, fragment_size_width_,
                                       num_fragments_),
      fragment_size_width_, size);
  payload_end_ = open_fragment_begin_ + size;
  ++num_fragments_;
  open_fragment_begin_ = kNoOpenFragment;

  return PublishHeldChunk(continues_on_next);
}

RingWriter::Outcome RingWriter::Release() {
  // An open fragment at this point is an abandoned one. Its bytes were never
  // counted, so nothing is published for them; the chunk itself still has to go
  // back to the ring.
  open_fragment_begin_ = kNoOpenFragment;

  if (ownership_ == Ownership::kAcquired)
    return PublishHeldChunk(/*continues_on_next=*/false);

  DropChunkHandle();
  return Outcome::kOk;
}

RingWriter::Outcome RingWriter::PublishHeldChunk(bool continues_on_next) {
  PERFETTO_DCHECK(ownership_ == Ownership::kAcquired);
  PERFETTO_DCHECK(!has_open_fragment());

  for (uint32_t attempt = 0;; ++attempt) {
    uint32_t flags = PayloadFlagsOf(state_word_);
    if (continues_on_next)
      flags |= kFlagContinuesOnNextChunk;
    const uint32_t complete_word =
        MakeDataBearingWord(ChunkState::kComplete, ChunkFormat::kTargetBuffer,
                            flags, num_fragments_, writer_id_);

    uint32_t observed = state_word_;
    if (ring_->TryPublish(chunk_index_, &observed, complete_word)) {
      state_word_ = complete_word;
      ownership_ = Ownership::kCompleteCached;
      // A Complete chunk carrying "continues on next chunk" is never reused.
      // Together with the rule that an Acquired word never carries that flag,
      // this is what guarantees that any non-empty published prefix the reader
      // can scrape ends on a packet boundary.
      if (continues_on_next || AvailableForNextFragment() == 0)
        DropChunkHandle();
      return Outcome::kOk;
    }

    // The reader marking our chunk is the only transition that can beat a
    // publication. Anything else means the word was changed by something that
    // had no right to, which is a bug on this side of the protocol and not
    // something a slow reader can produce.
    if (StateOf(observed) != ChunkState::kRewriteRequested ||
        WriterIdOf(observed) != writer_id_) {
      PERFETTO_FATAL(
          "tracing v2: publication of chunk %u by writer %u lost to state word "
          "0x%08x, which is not a rewrite request for this writer",
          chunk_index_, writer_id_, observed);
    }

    // The marked count proves which prefix the reader took. Everything above it
    // is still ours and has to move.
    const uint32_t taken = NumFragmentsOf(observed);
    PERFETTO_DCHECK(taken == NumFragmentsOf(state_word_));
    PERFETTO_DCHECK(taken <= num_fragments_);
    const uint32_t suffix_fragments = num_fragments_ - taken;

    // Copy the suffix out while the old ownership is still valid, i.e. before
    // the acknowledgement that releases the chunk. Both ranges: the payload
    // bytes and the matching size entries.
    uint32_t suffix_begin = kTargetBufferPayloadOffset;
    for (uint32_t i = 0; i < taken; ++i) {
      suffix_begin +=
          LoadFragmentSize(chunk_ + FragmentSizeEntryOffset(
                                        chunk_size_, fragment_size_width_, i),
                           fragment_size_width_);
    }
    relocation_payload_.assign(chunk_ + suffix_begin, chunk_ + payload_end_);
    relocation_sizes_.clear();
    for (uint32_t i = taken; i < num_fragments_; ++i) {
      relocation_sizes_.push_back(LoadFragmentSize(
          chunk_ +
              FragmentSizeEntryOffset(chunk_size_, fragment_size_width_, i),
          fragment_size_width_));
    }

    // Acknowledge before looking for replacement capacity. The other order
    // leaves the old chunk occupied whenever the ring is full, so every later
    // traversal of it burns a position. Acknowledge even if the suffix ends up
    // dropped: the acknowledgement is about the chunk, not about whether the
    // data survived.
    if (!ring_->TryAcknowledge(chunk_index_, observed)) {
      PERFETTO_FATAL(
          "tracing v2: writer %u could not acknowledge chunk %u; only its "
          "owner may leave RewriteRequested",
          writer_id_, chunk_index_);
    }
    ++num_relocations_;

    // If the reader took a non-empty prefix it took the chunk's beginning, so
    // the flags describing that beginning went out with it and must not be
    // repeated here. If it took nothing, they travel with the whole relocated
    // suffix.
    const uint32_t relocated_flags =
        taken == 0 ? PayloadFlagsOf(observed) &
                         (kFlagContinuesFromPrevChunk | kFlagDataLoss)
                   : 0;

    DropChunkHandle();

    if (suffix_fragments == 0) {
      // Everything we had finalized is already the reader's. A publication with
      // nothing left to move cannot have been carrying "continues on next
      // chunk": that flag describes a fragment we closed but had not published.
      PERFETTO_DCHECK(!continues_on_next);
      return Outcome::kOk;
    }

    const bool out_of_attempts = attempt + 1 >= kMaxRelocationAttempts;
    if (out_of_attempts || AcquireChunk(relocated_flags) != Outcome::kOk) {
      num_fragments_dropped_ += suffix_fragments;
      owes_data_loss_ = true;
      return Outcome::kRelocationDropped;
    }

    // Rebuild the suffix in the replacement. A suffix always fits: it came out
    // of one chunk and the replacement has the same geometry.
    memcpy(chunk_ + kTargetBufferPayloadOffset, relocation_payload_.data(),
           relocation_payload_.size());
    payload_end_ = kTargetBufferPayloadOffset +
                   static_cast<uint32_t>(relocation_payload_.size());
    for (uint32_t i = 0; i < relocation_sizes_.size(); ++i) {
      StoreFragmentSize(chunk_ + FragmentSizeEntryOffset(
                                     chunk_size_, fragment_size_width_, i),
                        fragment_size_width_, relocation_sizes_[i]);
    }
    num_fragments_ = static_cast<uint32_t>(relocation_sizes_.size());
    // Round again to publish the replacement, which the reader may also scrape.
  }
}

RingWriter::Outcome RingWriter::AcquireChunk(uint32_t carried_flags) {
  PERFETTO_DCHECK(ownership_ == Ownership::kNone);

  uint32_t flags = carried_flags;
  if (owes_data_loss_)
    flags |= kFlagDataLoss;

  uint32_t burned_claims = 0;
  uint32_t stall_slices = 0;
  for (;;) {
    const SharedRingBuffer::Reservation reservation = ring_->ReservePosition();

    if (reservation.outcome == SharedRingBuffer::ReserveOutcome::kFull) {
      // A hole this call just made is a position the reader has to resolve, and
      // it is part of why the ring now looks full. Report that rather than a
      // full ring, whatever the policy: it is what actually happened, and it is
      // what tells the caller that write_pos moved and the reader has to be
      // nudged.
      //
      // A stalling caller must not wait here either. The nudge happens after
      // this call returns, never from inside it, so waiting would be waiting
      // for a reader that this thread has made nobody responsible for waking.
      if (burned_claims != 0)
        return Outcome::kNoChunkAvailable;

      if (buffer_exhausted_policy_ == BufferExhaustedPolicy::kDrop)
        return Outcome::kFull;

      if (stall_slices >= kMaxStallSlices) {
        if (buffer_exhausted_policy_ == BufferExhaustedPolicy::kStall) {
          PERFETTO_FATAL(
              "tracing v2: writer %u stalled on a full ring for %u ms; "
              "possible deadlock",
              writer_id_, kMaxStallSlices * kStallSliceMs);
        }
        return Outcome::kFull;
      }

      // Wait on the exact sample the capacity decision was made against, so a
      // reader that frees space in between turns this into an immediate return
      // rather than a lost wakeup.
      const SharedRingBuffer::WaitOutcome wait = ring_->WaitForReadPosChange(
          reservation.read_pos_sample, kStallSliceMs);
      if (wait == SharedRingBuffer::WaitOutcome::kWaitUnavailable) {
        // Either there is no futex on this platform or the kernel refused the
        // wait. A stalling policy degrades to dropping rather than spinning on
        // a syscall that is not going to start working.
        return Outcome::kFull;
      }
      ++stall_slices;
      continue;
    }

    const uint32_t acquired_word =
        MakeDataBearingWord(ChunkState::kAcquired, ChunkFormat::kTargetBuffer,
                            flags, 0, writer_id_);
    if (ring_->TryClaim(reservation.position, acquired_word)) {
      chunk_index_ =
          ChunkIndexOfPosition(reservation.position, ring_->num_chunks());
      chunk_ = ring_->chunk_at(chunk_index_);
      state_word_ = acquired_word;
      ownership_ = Ownership::kAcquired;
      payload_end_ = kTargetBufferPayloadOffset;
      num_fragments_ = 0;
      owes_data_loss_ = false;
      // Nobody else may touch these bytes until the release publication out of
      // kAcquired, which is what makes them visible.
      StoreTargetBuffer(chunk_, target_buffer_);
      return Outcome::kOk;
    }

    // The claim lost, so this position is a hole that only the reader can
    // resolve. There is exactly one word this reservation was entitled to swap
    // against and it was not there, so the observed value is discarded and the
    // position is never retried; the loop goes back for a *later* one.
    ++num_holes_;
    if (++burned_claims >= kMaxBurnedClaims)
      return Outcome::kNoChunkAvailable;
  }
}

void RingWriter::DropChunkHandle() {
  ownership_ = Ownership::kNone;
  chunk_ = nullptr;
  chunk_index_ = 0;
  state_word_ = 0;
  payload_end_ = 0;
  num_fragments_ = 0;
  open_fragment_begin_ = kNoOpenFragment;
  open_fragment_end_ = 0;
}

}  // namespace perfetto::tracing_v2
