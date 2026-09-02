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
    result.last_result = ResolveNextPosition();
    if (result.last_result != ResolveResult::kChunkRead &&
        result.last_result != ResolveResult::kPositionSkipped) {
      break;
    }
  }

  result.positions_resolved = read_pos_ - start_pos;
  if (result.positions_resolved != 0) {
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
// 4. RewriteRequested: skip; the writer still owns the chunk. Once it becomes
//    RewriteAcknowledged, only the reader may reclaim it on a later traversal.
//
// In the first three cases, a lost CAS leaves read_pos unchanged for the next
// pass. Deliver the copy only after winning, so a retry cannot deliver the
// same fragments twice. RewriteAcknowledged has no competing writer
// transition; failure to reclaim it is a protocol error.
SharedRingBufferReader::ResolveResult
SharedRingBufferReader::ResolveNextPosition() {
  if (has_protocol_error_)
    return ResolveResult::kProtocolError;

  // A stale write_pos only shortens this drain pass.
  const uint32_t write_pos = ring_->LoadWritePos();
  const uint32_t outstanding = NumOutstandingPositions(write_pos, read_pos_);
  if (outstanding == 0)
    return ResolveResult::kNoData;
  if (outstanding > num_chunks_) {
    // A legal writer cannot reserve more than num_chunks outstanding
    // positions.
    has_protocol_error_ = true;
    PERFETTO_ELOG(
        "tracing v2: stopping ring reader; write_pos %u is %u positions ahead "
        "of read_pos %u, which is more than the %u chunks in the ring",
        write_pos, outstanding, read_pos_, num_chunks_);
    return ResolveResult::kProtocolError;
  }

  // read_pos_ is the next logical position.
  const uint32_t position = read_pos_;
  const uint32_t chunk_idx = ChunkIndexOf(position, num_chunks_);

  // A failed compare-and-swap replaces this with the word that won.
  uint32_t state_word = ring_->LoadChunkStateWord(chunk_idx);

  switch (ChunkStateOf(state_word)) {
    case ChunkState::kFree: {
      // Check reserved bits first; reclaiming must not hide an invalid word.
      if ((state_word & ~kWriterIDMask) != 0)
        return StopOnProtocolError("Free word has reserved bits", state_word);
      // Only this reader advances the wrap; a different wrap here is an error.
      const uint32_t expected_free_word =
          MakeFreeStateWordForPosition(position, num_chunks_);
      if (state_word != expected_free_word) {
        return StopOnProtocolError(
            "Free word carries another position's wrap count", state_word);
      }
      // Nobody claimed this reservation, so the reader advances the wrap
      // count. A writer can still claim between the load and this CAS. The
      // CAS then fails and the same position is retried as BeingWritten.
      if (!ring_->TryMoveFreeChunkToNextWrap(position, &state_word))
        return ResolveResult::kRetryLater;
      ++read_pos_;
      ++stats_.positions_skipped;
      return ResolveResult::kPositionSkipped;
    }

    case ChunkState::kBeingWritten: {
      const auto status = CopyCommittedPrefix(chunk_idx, state_word);
      // Validation does not settle ownership. Even a malformed prefix must win
      // the state transition before the reader can advance.
      if (!ring_->TryRequestRewrite(chunk_idx, &state_word))
        return ResolveResult::kRetryLater;
      ++read_pos_;
      ++stats_.rewrite_requests;
      return HandleCommittedPrefix(status);
    }

    case ChunkState::kComplete: {
      const auto status = CopyCommittedPrefix(chunk_idx, state_word);
      // The writer may have taken the chunk back, turning Complete(N) into
      // BeingWritten(N). The reader discards its copy and retries the same
      // position instead of delivering data from a lost race.
      if (!ring_->TryReleaseCompleteChunkAsFree(position, &state_word))
        return ResolveResult::kRetryLater;
      ++read_pos_;
      // A Complete chunk with no fragments can still carry kFlagDataLoss, and
      // this reclaim was the last chance to see it: the writer's reuse CAS
      // now fails and it forgets the chunk. The kBeingWritten case above
      // stays silent for the same shape because that chunk still has an
      // owner, which moves the flag to the relocated suffix.
      if (status == CommittedPrefixStatus::kNoFragments &&
          (copied_chunk_.payload_flags & kFlagDataLoss)) {
        delegate_->OnDataLoss(copied_chunk_.writer_id);
      }
      return HandleCommittedPrefix(status);
    }

    case ChunkState::kRewriteRequested:
      // The writer still owns this chunk. Resolve the position as a hole.
      ++read_pos_;
      ++stats_.positions_skipped;
      return ResolveResult::kPositionSkipped;

    case ChunkState::kRewriteAcknowledged:
      // RewriteAcknowledged has one canonical word with no payload fields, and
      // the reclaim compares against exactly that word. After acknowledging,
      // the writer is finished with the chunk and only this reader may change
      // it, so a failed reclaim cannot be a lost race: either the word was not
      // canonical or something other than this reader moved it.
      if (!ring_->TryReleaseRewriteAcknowledgedChunkAsFree(position,
                                                           &state_word)) {
        return StopOnProtocolError(
            ChunkStateOf(state_word) == ChunkState::kRewriteAcknowledged
                ? "RewriteAcknowledged word has payload bits set"
                : "RewriteAcknowledged word changed under the reader",
            state_word);
      }
      ++read_pos_;
      ++stats_.positions_skipped;
      return ResolveResult::kPositionSkipped;

    case ChunkState::kReserved5:
    case ChunkState::kReserved6:
    case ChunkState::kReserved7:
      // The reader cannot safely reclaim an unknown state.
      return StopOnProtocolError("reserved chunk state", state_word);
  }
  PERFETTO_FATAL("tracing v2: unhandled chunk state word 0x%08x", state_word);
}

SharedRingBufferReader::CommittedPrefixStatus
SharedRingBufferReader::CopyCommittedPrefix(uint32_t chunk_idx,
                                            uint32_t state_word) {
  copied_fragments_.clear();
  copied_chunk_ = ChunkContents{};
  copied_chunk_.writer_id = WriterIDOf(state_word);
  copied_chunk_.payload_flags = PayloadFlagsOf(state_word);

  const uint32_t num_fragments = NumFragmentsOf(state_word);
  if (num_fragments == 0) {
    // A writer may still be storing BufferID after claiming BeingWritten(0).
    // Only its first release publication makes that store visible. Do not
    // touch any bytes beyond the state word when no fragment is published.
    return CommittedPrefixStatus::kNoFragments;
  }

  if (ChunkFormatOf(state_word) != ChunkFormat::kTargetBuffer)
    return CommittedPrefixStatus::kUnsupportedFormat;

  // Decode and validate the fragment sizes.
  const uint32_t capacity = chunk_size_ - kTargetBufferPayloadOffset;
  const uint8_t* chunk = ring_->chunk_at(chunk_idx);
  const uint8_t* const payload_begin = chunk + kTargetBufferPayloadOffset;
  const uint8_t* sizes_cursor = chunk + chunk_size_;
  uint32_t total = 0;
  for (uint32_t i = 0; i < num_fragments; ++i) {
    uint32_t fragment_size = 0;
    if (!ReadFragmentSize(payload_begin, &sizes_cursor, &fragment_size) ||
        fragment_size > capacity - total) {
      return CommittedPrefixStatus::kMalformed;
    }
    total += fragment_size;
    copied_fragments_.push_back(Fragment{nullptr, fragment_size});
  }

  const uint32_t sizes_bytes =
      static_cast<uint32_t>(chunk + chunk_size_ - sizes_cursor);
  if (total > capacity - sizes_bytes)
    return CommittedPrefixStatus::kMalformed;

  // Copy the published payload out of shared memory.
  copied_payload_.assign(payload_begin, payload_begin + total);

  // Point each Fragment at its copy and collect the chunk metadata.
  uint32_t offset = 0;
  for (Fragment& fragment : copied_fragments_) {
    fragment.data = copied_payload_.data() + offset;
    offset += fragment.size;
  }
  copied_chunk_.target_buffer = LoadTargetBufferID(chunk);
  copied_chunk_.fragments = copied_fragments_.data();
  copied_chunk_.num_fragments = num_fragments;
  return CommittedPrefixStatus::kReady;
}

SharedRingBufferReader::ResolveResult
SharedRingBufferReader::HandleCommittedPrefix(CommittedPrefixStatus status) {
  switch (status) {
    case CommittedPrefixStatus::kReady:
      ++stats_.chunks_read;
      delegate_->OnChunkRead(copied_chunk_);
      return ResolveResult::kChunkRead;
    case CommittedPrefixStatus::kMalformed:
      ++stats_.malformed_chunks;
      delegate_->OnDataLoss(copied_chunk_.writer_id);
      break;
    case CommittedPrefixStatus::kUnsupportedFormat:
      ++stats_.unsupported_format_chunks;
      delegate_->OnDataLoss(copied_chunk_.writer_id);
      break;
    case CommittedPrefixStatus::kNoFragments:
      break;
  }

  ++stats_.positions_skipped;
  return ResolveResult::kPositionSkipped;
}

SharedRingBufferReader::ResolveResult
SharedRingBufferReader::StopOnProtocolError(const char* reason,
                                            uint32_t state_word) {
  // Stop rather than trusting a malformed word from a producer. Log once.
  has_protocol_error_ = true;
  PERFETTO_ELOG(
      "tracing v2: stopping ring reader at position %u: %s (chunk state word "
      "0x%08x, %s)",
      read_pos_, reason, state_word, ChunkStateName(ChunkStateOf(state_word)));
  return ResolveResult::kProtocolError;
}

}  // namespace perfetto::tracing_v2
