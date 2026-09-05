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
  copied_payload_.reserve(MaxFragmentSizeForEmptyChunk(chunk_size_));
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
    // TODO(sashwinbalaji): benchmark per-position publication against batch
    // sizes such as 16, 64 and 256 before trusting any of them. Nothing here
    // establishes that the caller's current pass size is optimal.
    ring_->PublishReadPos(read_pos_);
  }
  return result;
}

// Writers keep running while the reader resolves one position. The chunk for
// that position moves along the writer's row in the sketch below, and the
// reader's own transitions are the vertical arrows. Every reader transition
// is an exact-value compare-and-swap. If the writer moves first, the reader's
// CAS fails and the position is retried in its new state.
//
//                writer claims               writer publishes
//   Free(wrap)  -------------->  BeingWritten(N)  -------------->  Complete(M)
//       |                              |                               |
//       | reader                       | reader                        | reader
//       v                              v                               v
//   Free(next wrap)           RewriteRequested(N)               Free(next wrap)
//                                      |
//                                      | writer finishes with old chunk
//                                      v
//                             RewriteAcknowledged
//                                      |
//                                      | reader, on a later traversal
//                                      v
//                               Free(next wrap)
//
// The races this function has to handle:
//
// 1. Free versus the delayed writer's claim. The writer reserved this position
//    but has not claimed the chunk. The reader retires the position by moving
//    the Free word to the next traversal. If the writer's claim lands first,
//    the reader's CAS fails and it retries this position. If the reader lands
//    first, the writer's claim fails and it reserves a later position.
// 2. BeingWritten versus the writer publishing Complete. The reader copies the
//    N published fragments, then races BeingWritten(N) -> RewriteRequested(N)
//    against the writer's BeingWritten(N) -> Complete(M).
// 3. Complete versus the writer taking its cached chunk back. The reader
//    copies the M fragments, then races Complete(M) -> Free against the
//    writer's Complete(M) -> BeingWritten(M).
// 4. RewriteRequested and RewriteAcknowledged cleanup. A RewriteRequested
//    chunk still belongs to its writer, so the position is a hole. A
//    RewriteAcknowledged chunk is one its writer has finished with, and only
//    the reader may move it to Free.
//
// Copied bytes are speculative until the reader's CAS succeeds. The delegate
// sees a copy only after that, so a lost race never delivers data twice.
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

  // read_pos_ is the next logical position. For a four-chunk ring, positions
  // 0, 4 and 8 all use chunk 0, with Free tags 0, 1 and 2 respectively.
  const uint32_t position = read_pos_;
  const uint32_t chunk_idx = ChunkIndexOfPosition(position, num_chunks_);
  const uint32_t expected_free_word =
      MakeFreeStateWord(WrapCountForPosition(position, num_chunks_));

  // A failed compare-and-swap replaces this with the word that won.
  uint32_t state_word = ring_->LoadChunkStateWord(chunk_idx);

  switch (ChunkStateOf(state_word)) {
    case ChunkState::kFree:
      // A Free tag for a different traversal is invalid at this position.
      if (state_word != expected_free_word)
        return StopOnProtocolError(state_word);
      // Nobody claimed this reservation, so the reader advances the tag. A
      // writer can still claim between the load and this CAS. The CAS then
      // fails and the same position is retried as BeingWritten.
      if (!ring_->TryMoveFreeChunkToNextWrap(position, &state_word))
        return ResolveResult::kRetryLater;
      ++read_pos_;
      ++stats_.positions_skipped;
      return ResolveResult::kPositionSkipped;

    case ChunkState::kBeingWritten: {
      const CommittedPrefixStatus status =
          CopyCommittedPrefix(chunk_idx, state_word);
      // Validation does not settle ownership. Even a malformed prefix must win
      // the state transition before the reader can advance.
      if (!ring_->TryRequestRewrite(chunk_idx, &state_word))
        return ResolveResult::kRetryLater;
      ++read_pos_;
      ++stats_.rewrite_requests;
      return HandleCommittedPrefix(status);
    }

    case ChunkState::kComplete: {
      const CommittedPrefixStatus status =
          CopyCommittedPrefix(chunk_idx, state_word);
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
      // After acknowledging, the writer is finished with the chunk and only
      // this reader may change the word. So a failed reclaim cannot be a lost
      // race. It is a protocol error.
      if (!ring_->TryReleaseRewriteAcknowledgedChunkAsFree(position,
                                                           &state_word)) {
        return StopOnProtocolError(state_word);
      }
      ++read_pos_;
      ++stats_.positions_skipped;
      return ResolveResult::kPositionSkipped;

    case ChunkState::kReserved5:
    case ChunkState::kReserved6:
    case ChunkState::kReserved7:
      // The reader cannot safely reclaim an unknown state.
      return StopOnProtocolError(state_word);
  }
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
    // Nothing is published, so there is nothing to copy. Nothing else in the
    // chunk may be touched either. The writer stores the target BufferID while
    // it exclusively owns a freshly claimed chunk, and only the first release
    // transition out of kBeingWritten publishes that store. A BeingWritten
    // word with no fragments therefore does not order it against this reader
    // at all.
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
      copied_fragments_.clear();
      return CommittedPrefixStatus::kMalformed;
    }
    total += fragment_size;
    copied_fragments_.push_back(Fragment{nullptr, fragment_size});
  }

  const uint32_t sizes_bytes =
      static_cast<uint32_t>(chunk + chunk_size_ - sizes_cursor);
  if (total > capacity - sizes_bytes) {
    copied_fragments_.clear();
    return CommittedPrefixStatus::kMalformed;
  }

  // Copy the published payload out of shared memory.
  copied_payload_.assign(chunk + kTargetBufferPayloadOffset,
                         chunk + kTargetBufferPayloadOffset + total);

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
SharedRingBufferReader::StopOnProtocolError(uint32_t state_word) {
  // Stop rather than trusting a malformed word from a producer. Log once.
  has_protocol_error_ = true;
  PERFETTO_ELOG(
      "tracing v2: stopping ring reader at position %u; chunk state word "
      "0x%08x (%s) is not something this build can arbitrate",
      read_pos_, state_word, ChunkStateName(ChunkStateOf(state_word)));
  return ResolveResult::kProtocolError;
}

}  // namespace perfetto::tracing_v2
