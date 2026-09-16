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

#include "src/tracing/v2/shared_ring_buffer_reader.h"

#include <stdint.h>

#include "perfetto/base/logging.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"

namespace perfetto::tracing_v2 {
namespace {

// Bound attempts at one position. For M = kMaxFragmentsPerChunk:
// - Each fragment can defeat the reader's CAS twice:
//   1. Publication: BeingWritten(N) -> Complete(N + 1).
//   2. Reuse: Complete(N + 1) -> BeingWritten(N + 1).
//   Allow M attempts for each, giving 2 * M.
// - Add one for the initial Free -> BeingWritten claim.
// - Add one for the reader's successful CAS, giving 2 * M + 2.
//
// This is conservative. SharedRingBufferWriter stops reusing at M fragments,
// so there are at most M - 1 reuses. Zero-byte fragments still use the count.
// FinishCurrentChunk() drops the cached handle even when abandoning a fragment.
// The limit also bounds work if another producer keeps changing the state
// without increasing the fragment count.
constexpr uint32_t kMaxAttemptsPerPosition = 2 * kMaxFragmentsPerChunk + 2;

// For the protocol-error log only.
const char* ChunkStateName(ChunkState state) {
  switch (state) {
    case ChunkState::kFree:
      return "Free";
    case ChunkState::kBeingWritten:
      return "BeingWritten";
    case ChunkState::kComplete:
      return "Complete";
    case ChunkState::kRewriteRequested:
      return "RewriteRequested";
    case ChunkState::kRewriteAcknowledged:
      return "RewriteAcknowledged";
    case ChunkState::kReserved5:
    case ChunkState::kReserved6:
    case ChunkState::kReserved7:
      return "reserved";
  }
  return "unknown";
}

}  // namespace

SharedRingBufferReader::Delegate::~Delegate() = default;

SharedRingBufferReader::SharedRingBufferReader(SharedRingBuffer* ring,
                                               Delegate* delegate)
    : ring_(ring),
      delegate_(delegate),
      num_chunks_(ring->num_chunks()),
      chunk_size_(ring->chunk_size()) {
  // Reserve enough scratch for any chunk format so draining does not allocate.
  copied_payload_.reserve(chunk_size_);
  copied_fragments_.reserve(kMaxFragmentsPerChunk);
}

SharedRingBufferReader::~SharedRingBufferReader() = default;

SharedRingBufferReader::DrainResult SharedRingBufferReader::Drain(
    uint32_t max_positions) {
  const uint32_t start_pos = read_pos_;
  DrainResult result{};
  for (uint32_t i = 0; i < max_positions; ++i) {
    // A failed CAS leaves read_pos_ unchanged. Retry the same position with
    // a fresh state and fragment copy before it counts towards max_positions.
    // Limit attempts so repeated writer updates cannot monopolize this pass.
    for (uint32_t attempt = 0; attempt < kMaxAttemptsPerPosition; ++attempt) {
      result.last_result = ConsumeNextPosition();
      if (result.last_result != ConsumeResult::kRetryImmediately)
        break;
    }
    // If every attempt lost, yield with another drain requested. Contention
    // alone is not a protocol error. Publish earlier progress below.
    if (result.last_result != ConsumeResult::kChunkRead &&
        result.last_result != ConsumeResult::kPositionSkipped) {
      break;
    }
  }

  // Unsigned subtraction also counts positions across uint32_t rollover.
  // Advancing from UINT32_MAX to zero consumes one position.
  result.positions_consumed = read_pos_ - start_pos;
  if (result.positions_consumed != 0) {
    // One publication and at most one wake cover the whole pass. Until this
    // point writers can only under-estimate free capacity.
    //
    // TODO(sashwinbalaji): benchmark useful publication batch sizes.
    ring_->PublishReadPos(read_pos_);
  }
  return result;
}

// The shared lifecycle is in shared_ring_buffer_abi.h.
//
// The races this function has to handle:
//
// 1. Free: advance to the next wrap, racing the delayed writer's claim.
// 2. BeingWritten: copy N fragments, then request RewriteRequested(N), racing
//    the writer's publication of Complete(M).
// 3. Complete: copy the fragments, then reclaim, racing the writer's reuse.
// 4. RewriteRequested: skip. The writer still owns the chunk. Once it becomes
//    RewriteAcknowledged, only the reader may reclaim it on a later traversal.
//
// In the first three cases, a failed CAS leaves read_pos unchanged. Drain()
// retries immediately. Deliver the copy only after a successful transition,
// so a retry cannot deliver the same fragments twice.
//
// RewriteAcknowledged has no competing writer transition. On failure to reclaim
// it, report a protocol error.
SharedRingBufferReader::ConsumeResult
SharedRingBufferReader::ConsumeNextPosition() {
  if (PERFETTO_UNLIKELY(has_protocol_error_))
    return ConsumeResult::kProtocolError;

  // A relaxed load is enough here. An older write_pos only shortens this
  // drain pass, which is still correct. A later pass can consume the rest.
  const uint32_t write_pos = ring_->LoadWritePosRelaxed();
  const uint32_t outstanding = NumOutstandingPositions(write_pos, read_pos_);
  if (outstanding == 0)
    return ConsumeResult::kNoData;
  if (PERFETTO_UNLIKELY(outstanding > num_chunks_)) {
    // A legal writer cannot reserve more than num_chunks outstanding
    // positions.
    has_protocol_error_ = true;
    PERFETTO_ELOG(
        "tracing v2: stopping ring buffer reader: write_pos %u is %u "
        "positions ahead of read_pos %u, which is more than the %u chunks "
        "in the ring buffer",
        write_pos, outstanding, read_pos_, num_chunks_);
    return ConsumeResult::kProtocolError;
  }

  // read_pos_ is the next logical position.
  const uint32_t chunk_pos = read_pos_;
  const ChunkIndex chunk_idx = ChunkIndex::FromPosition(chunk_pos, num_chunks_);

  // A failed compare-and-swap replaces this with the word that won.
  uint32_t state_word = ring_->LoadChunkStateWordAcquire(chunk_idx);

  switch (ChunkStateOf(state_word)) {
    case ChunkState::kFree: {
      // Check reserved bits first. Reclaiming must not hide an invalid word.
      if (PERFETTO_UNLIKELY((state_word & ~kWriterIDMask) != 0))
        return StopOnProtocolError("Free word has reserved bits", state_word);
      // Only this reader advances the wrap. A different wrap here is an error.
      const uint32_t expected_free_word =
          MakeFreeStateWordForPosition(chunk_pos, num_chunks_);
      if (PERFETTO_UNLIKELY(state_word != expected_free_word)) {
        return StopOnProtocolError(
            "Free word carries another position's wrap count", state_word);
      }
      if (before_state_transition_for_testing_)
        before_state_transition_for_testing_();
      // Advance the wrap count to consume this unclaimed reservation.
      // On failure, the delayed writer claimed first. Drain() retries this
      // position with the writer's BeingWritten or Complete state.
      if (!ring_->TryMoveFreeChunkToNextWrap(chunk_pos, &state_word))
        return ConsumeResult::kRetryImmediately;
      ++read_pos_;
      ++stats_.positions_skipped;
      return ConsumeResult::kPositionSkipped;
    }

    case ChunkState::kBeingWritten: {
      const auto status = CopyPublishedFragments(chunk_idx, state_word);
      if (before_state_transition_for_testing_)
        before_state_transition_for_testing_();
      // Request a rewrite before read_pos_ advances, even if validation failed.
      // On failure, the writer published first. Drain() retries with a fresh
      // copy before it delivers fragments or reports loss for this position.
      if (!ring_->TryRequestRewrite(chunk_idx, &state_word))
        return ConsumeResult::kRetryImmediately;
      ++read_pos_;
      ++stats_.rewrite_requests;
      return HandleCopiedChunk(status);
    }

    case ChunkState::kComplete: {
      const auto status = CopyPublishedFragments(chunk_idx, state_word);
      if (before_state_transition_for_testing_)
        before_state_transition_for_testing_();
      // Reclaim the chunk before read_pos_ advances, even if validation failed.
      // On failure, the writer reused the chunk and can publish more fragments.
      // Drain() reloads the state and copies the current published fragments.
      // If the state is BeingWritten, that attempt requests a rewrite.
      // Deliver fragments or report loss only after a successful transition.
      if (!ring_->TryReleaseCompleteChunkAsFree(chunk_pos, &state_word))
        return ConsumeResult::kRetryImmediately;
      ++read_pos_;
      // Report loss even if the Complete chunk has no fragments. The writer
      // cannot reuse the reclaimed chunk, so no later publication can report
      // its flag.
      //
      // For BeingWritten(0), the writer still owns the chunk and transfers
      // kFlagDataLoss to the relocated fragment instead.
      if (status == CopiedChunkStatus::kNoFragments &&
          (copied_chunk_.payload_flags & kFlagDataLoss)) {
        delegate_->OnDataLoss(copied_chunk_.writer_id);
      }
      return HandleCopiedChunk(status);
    }

    case ChunkState::kRewriteRequested:
      // This chunk is still awaiting rewrite acknowledgement from its owner.
      // The current reservation could not claim it. Advance read_pos without
      // changing the chunk's state word or delivering fragments.
      ++read_pos_;
      ++stats_.positions_skipped;
      return ConsumeResult::kPositionSkipped;

    case ChunkState::kRewriteAcknowledged:
      // Reclaim the chunk before advancing past this unclaimed reservation.
      // No fragments are delivered. The chunk becomes available for a later
      // reservation.
      //
      // The expected word contains only the RewriteAcknowledged state bits.
      // Only this reader can change it after the writer's acknowledgement.
      // On failure, the word was invalid or another actor changed it.
      if (!ring_->TryReleaseRewriteAcknowledgedChunkAsFree(chunk_pos,
                                                           &state_word)) {
        return StopOnProtocolError(
            ChunkStateOf(state_word) == ChunkState::kRewriteAcknowledged
                ? "RewriteAcknowledged word has payload bits set"
                : "RewriteAcknowledged word changed under the reader",
            state_word);
      }
      ++read_pos_;
      ++stats_.positions_skipped;
      return ConsumeResult::kPositionSkipped;

    case ChunkState::kReserved5:
    case ChunkState::kReserved6:
    case ChunkState::kReserved7:
      // The reader cannot safely reclaim an unknown state.
      return StopOnProtocolError("reserved chunk state", state_word);
  }
  PERFETTO_FATAL("tracing v2: unhandled chunk state word 0x%08x", state_word);
}

SharedRingBufferReader::CopiedChunkStatus
SharedRingBufferReader::CopyPublishedFragments(ChunkIndex chunk_idx,
                                               uint32_t state_word) {
  copied_fragments_.clear();
  copied_chunk_ = ChunkContents{};
  copied_chunk_.writer_id = WriterIDOf(state_word);
  copied_chunk_.payload_flags = PayloadFlagsOf(state_word);

  const uint32_t num_fragments = NumFragmentsOf(state_word);
  if (num_fragments == 0) {
    // A writer may still be storing BufferID after claiming BeingWritten(0).
    // Only its first fragment publication makes that store visible. Do not
    // touch any bytes beyond the state word when no fragment is published.
    return CopiedChunkStatus::kNoFragments;
  }

  // The loss flag does not identify where data was lost within the chunk.
  // - Drop every published fragment, even a complete packet after the gap.
  // - Do not read an unpublished fragment. The writer may relocate it.
  // The caller must win the state transition before reporting the gap or
  // advancing read_pos. See Delegate::OnDataLoss() for packet recovery.
  if (state_word & kFlagDataLoss)
    return CopiedChunkStatus::kDataLoss;

  if (ChunkFormatOf(state_word) != ChunkFormat::kTargetBuffer)
    return CopiedChunkStatus::kUnsupportedFormat;

  // Decode first to find the payload end and the directory start.
  // Until then, payload_begin is the decoder's lower bound. It prevents size
  // reads from reaching the header, but does not prove the payload fits.
  const uint32_t capacity = chunk_size_ - kTargetBufferPayloadOffset;
  const uint8_t* chunk = ring_->chunk_at(chunk_idx);
  const uint8_t* const payload_begin = chunk + kTargetBufferPayloadOffset;
  const uint8_t* sizes_cursor = chunk + chunk_size_;
  uint32_t total = 0;
  for (uint32_t i = 0; i < num_fragments; ++i) {
    const auto fragment_size =
        ReadFragmentSizeReversed(payload_begin, &sizes_cursor);
    if (!fragment_size || *fragment_size > capacity - total) {
      return CopiedChunkStatus::kMalformed;
    }
    total += *fragment_size;
    copied_fragments_.push_back(Fragment{nullptr, *fragment_size});
  }

  // The sizes are valid varints. Their payloads may still overlap the
  // directory:
  // - Count the bytes actually read, including non-minimal size encodings.
  // - On overlap, reject the chunk before copying or delivering any payload.
  // - sizes_cursor is local to this read attempt. Returning discards it.
  //   No cursor change needs to be undone.
  const uint32_t sizes_bytes =
      static_cast<uint32_t>(chunk + chunk_size_ - sizes_cursor);
  if (total > capacity - sizes_bytes)
    return CopiedChunkStatus::kMalformed;

  // Copy the published payload out of shared memory.
  copied_payload_.assign(payload_begin, payload_begin + total);

  // Point each Fragment at its copy and collect the chunk metadata.
  uint32_t offset = 0;
  for (Fragment& fragment : copied_fragments_) {
    fragment.data = copied_payload_.data() + offset;
    offset += fragment.size;
  }
  copied_chunk_.target_buffer = LoadTargetBufferId(chunk);
  copied_chunk_.fragments = copied_fragments_.data();
  copied_chunk_.num_fragments = num_fragments;
  return CopiedChunkStatus::kReady;
}

SharedRingBufferReader::ConsumeResult SharedRingBufferReader::HandleCopiedChunk(
    CopiedChunkStatus status) {
  switch (status) {
    case CopiedChunkStatus::kReady:
      ++stats_.chunks_read;
      delegate_->OnChunkRead(copied_chunk_);
      return ConsumeResult::kChunkRead;
    case CopiedChunkStatus::kMalformed:
      ++stats_.malformed_chunks;
      delegate_->OnDataLoss(copied_chunk_.writer_id);
      break;
    case CopiedChunkStatus::kUnsupportedFormat:
      ++stats_.unsupported_format_chunks;
      delegate_->OnDataLoss(copied_chunk_.writer_id);
      break;
    case CopiedChunkStatus::kDataLoss:
      delegate_->OnDataLoss(copied_chunk_.writer_id);
      break;
    case CopiedChunkStatus::kNoFragments:
      break;
  }

  ++stats_.positions_skipped;
  return ConsumeResult::kPositionSkipped;
}

SharedRingBufferReader::ConsumeResult
SharedRingBufferReader::StopOnProtocolError(const char* reason,
                                            uint32_t state_word) {
  // Stop rather than trusting a malformed word from a producer. Log once.
  has_protocol_error_ = true;
  PERFETTO_ELOG(
      "tracing v2: stopping ring buffer reader at position %u: %s "
      "(chunk state word 0x%08x, %s)",
      read_pos_, reason, state_word, ChunkStateName(ChunkStateOf(state_word)));
  return ConsumeResult::kProtocolError;
}

}  // namespace perfetto::tracing_v2
