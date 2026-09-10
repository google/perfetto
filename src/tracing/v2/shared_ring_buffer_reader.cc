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

#include "perfetto/base/compiler.h"
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
    result.last_result = ConsumeNextPosition();
    if (result.last_result != ConsumeResult::kChunkRead &&
        result.last_result != ConsumeResult::kPositionSkipped) {
      break;
    }
  }

  result.positions_consumed = read_pos_ - start_pos;
  if (result.positions_consumed != 0) {
    // uint32_t subtraction gives the forward distance across wraparound.
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
// A lost CAS leaves read_pos unchanged. BeingWritten retries locally; Free
// and Complete retry on a later pass. Deliver the copy only after winning,
// so a retry cannot deliver the same fragments twice. RewriteAcknowledged has
// no competing writer transition; failure to reclaim it is a protocol error.
SharedRingBufferReader::ConsumeResult
SharedRingBufferReader::ConsumeNextPosition() {
  if (has_protocol_error_)
    return ConsumeResult::kProtocolError;

  // The cursor bounds reservations; the chunk state publishes their payload.
  const uint32_t write_pos = ring_->LoadWritePos();
  const uint32_t outstanding = NumOutstandingPositions(write_pos, read_pos_);
  if (outstanding == 0)
    return ConsumeResult::kNoData;
  if (PERFETTO_UNLIKELY(outstanding > num_chunks_)) {
    // A legal writer cannot reserve more than num_chunks outstanding
    // positions.
    has_protocol_error_ = true;
    PERFETTO_ELOG(
        "tracing v2: stopping ring buffer reader; write_pos %u is "
        "%u positions ahead of read_pos %u, but the ring buffer has only %u "
        "chunks",
        write_pos, outstanding, read_pos_, num_chunks_);
    return ConsumeResult::kProtocolError;
  }

  // read_pos_ is the next logical position.
  const uint32_t chunk_pos = read_pos_;
  const auto chunk_idx = ChunkIndex::FromPosition(chunk_pos, num_chunks_);

  for (;;) {
    uint32_t state_word = ring_->LoadChunkStateWord(chunk_idx);

    switch (ChunkStateOf(state_word)) {
      case ChunkState::kFree: {
        // Check reserved bits first; reclaiming must not hide an invalid word.
        if ((state_word & ~kWriterIDMask) != 0)
          return StopOnProtocolError("Free word has reserved bits", state_word);
        // Only this reader advances the wrap. A different wrap is an error.
        const uint32_t expected_free_word =
            MakeFreeStateWordForPosition(chunk_pos, num_chunks_);
        if (state_word != expected_free_word) {
          return StopOnProtocolError(
              "Free word carries another position's wrap count", state_word);
        }
        // Nobody claimed this reservation, so the reader advances the wrap
        // count. A writer can still claim between the load and this CAS. The
        // CAS then fails and the same position is retried as BeingWritten.
        if (!ring_->TryMoveFreeChunkToNextWrap(chunk_pos, &state_word))
          return ConsumeResult::kRetryLater;
        ++read_pos_;
        ++stats_.positions_skipped;
        return ConsumeResult::kPositionSkipped;
      }

      case ChunkState::kBeingWritten: {
        const auto status = CopyCommittedPrefix(chunk_idx, state_word);
        // Validation does not settle ownership. Even a malformed prefix must
        // win the state transition before the reader can advance.
        if (before_rewrite_for_testing_)
          before_rewrite_for_testing_();
        if (!ring_->TryRequestRewrite(chunk_idx, &state_word)) {
          // The writer published a newer prefix. Discard this copy and reload
          // the state to try again. Each further publication increases the
          // bounded fragment count; if the writer stops, the next CAS wins.
          continue;
        }
        ++read_pos_;
        ++stats_.rewrite_requests;
        return HandleCommittedPrefix(status);
      }

      case ChunkState::kComplete: {
        const auto status = CopyCommittedPrefix(chunk_idx, state_word);
        // The writer may have taken the chunk back, turning Complete(N) into
        // BeingWritten(N). The reader discards its copy and retries the same
        // position instead of delivering data from a lost race.
        if (!ring_->TryReleaseCompleteChunkAsFree(chunk_pos, &state_word))
          return ConsumeResult::kRetryLater;
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
        // The writer still owns this chunk. Consume the position as a hole.
        ++read_pos_;
        ++stats_.positions_skipped;
        return ConsumeResult::kPositionSkipped;

      case ChunkState::kRewriteAcknowledged:
        // RewriteAcknowledged has one canonical word with no payload fields,
        // and the reclaim compares against exactly that word. After
        // acknowledging, the writer is finished with the chunk and only this
        // reader may change it, so a failed reclaim cannot be a lost race:
        // either the word was not canonical or something other than this reader
        // moved it.
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
}

SharedRingBufferReader::CommittedPrefixStatus
SharedRingBufferReader::CopyCommittedPrefix(ChunkIndex chunk_idx,
                                            uint32_t state_word) {
  copied_fragments_.clear();
  copied_chunk_ = ChunkContents{};
  copied_chunk_.writer_id = WriterIDOf(state_word);
  copied_chunk_.payload_flags = PayloadFlagsOf(state_word);

  const uint32_t num_fragments = NumFragmentsOf(state_word);
  if (num_fragments == 0) {
    // A writer may still be storing BufferID after claiming BeingWritten(0).
    // Its first publication makes that store visible. Until then, do not touch
    // any bytes beyond the state word.
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
    const auto fragment_size =
        ReadFragmentSizeReversed(payload_begin, &sizes_cursor);
    if (!fragment_size || *fragment_size > capacity - total) {
      return CommittedPrefixStatus::kMalformed;
    }
    total += *fragment_size;
    copied_fragments_.push_back(Fragment{nullptr, *fragment_size});
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

SharedRingBufferReader::ConsumeResult
SharedRingBufferReader::HandleCommittedPrefix(CommittedPrefixStatus status) {
  switch (status) {
    case CommittedPrefixStatus::kReady:
      ++stats_.chunks_read;
      delegate_->OnChunkRead(copied_chunk_);
      return ConsumeResult::kChunkRead;
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
