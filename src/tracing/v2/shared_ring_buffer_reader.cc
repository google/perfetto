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

#include <utility>

#include "perfetto/base/logging.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/tracing_v2_abi.h"

namespace perfetto::tracing_v2 {

SharedRingBufferReader::Delegate::~Delegate() = default;

SharedRingBufferReader::SharedRingBufferReader(SharedRingBuffer* ring,
                                               Delegate* delegate)
    : ring_(ring),
      delegate_(delegate),
      num_chunks_(ring->num_chunks()),
      chunk_size_(ring->chunk_size()),
      chunk_index_bits_(ring->chunk_index_bits()) {
  copied_payload_.reserve(MaxFragmentSizeForEmptyChunk(chunk_size_));
  copied_fragments_.reserve(kMaxFragmentsPerChunk);
}

SharedRingBufferReader::~SharedRingBufferReader() = default;

SharedRingBufferReader::DrainResult SharedRingBufferReader::Drain(
    uint32_t max_positions) {
  const uint32_t start_pos = read_pos_;
  DrainResult result{};
  for (uint32_t i = 0; i < max_positions; ++i) {
    result.last_outcome = ResolveNextPosition();
    if (result.last_outcome != Outcome::kChunkRead &&
        result.last_outcome != Outcome::kSkipped) {
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

SharedRingBufferReader::Outcome SharedRingBufferReader::ResolveNextPosition() {
  if (has_protocol_error_)
    return Outcome::kProtocolError;

  // A stale write_pos only shortens this drain pass.
  const uint32_t write_pos = ring_->LoadWritePos();
  const uint32_t outstanding = NumOutstandingPositions(write_pos, read_pos_);
  if (outstanding == 0)
    return Outcome::kNoData;
  if (outstanding > num_chunks_) {
    // A legal writer cannot reserve more than num_chunks outstanding
    // positions.
    has_protocol_error_ = true;
    PERFETTO_ELOG(
        "tracing v2: stopping ring reader; write_pos %u is %u positions ahead "
        "of read_pos %u, which is more than the %u chunks in the ring",
        write_pos, outstanding, read_pos_, num_chunks_);
    return Outcome::kProtocolError;
  }

  const uint32_t position = read_pos_;
  const uint32_t chunk_index = ChunkIndexOfPosition(position, num_chunks_);

  // A failed compare-and-swap replaces this with the current state word.
  uint32_t observed = ring_->LoadChunkStateWord(chunk_index);

  switch (ChunkStateOf(observed)) {
    case ChunkState::kFree: {
      if (observed != MakeFreeStateWord(
                          WrapCountForPosition(position, chunk_index_bits_))) {
        return StopOnProtocolError(observed);
      }
      // Nobody claimed this position. Prepare the chunk for its next wrap.
      if (!ring_->TryMoveFreeChunkToNextWrap(position, &observed))
        return Outcome::kRetryLater;
      ++read_pos_;
      ++num_positions_skipped_;
      return Outcome::kSkipped;
    }

    case ChunkState::kBeingWritten: {
      // This copy is speculative until the CAS below wins against the writer.
      const CommittedPrefixStatus status =
          CopyCommittedPrefix(chunk_index, observed);
      if (before_arbitration_hook_for_testing_)
        before_arbitration_hook_for_testing_();
      // Malformed payload is still marked: validation does not decide
      // ownership.
      if (!ring_->TryRequestRewrite(chunk_index, &observed))
        return Outcome::kRetryLater;
      ++read_pos_;
      ++num_scrapes_;
      return ForwardCopiedChunk(status);
    }

    case ChunkState::kComplete: {
      const CommittedPrefixStatus status =
          CopyCommittedPrefix(chunk_index, observed);
      if (before_arbitration_hook_for_testing_)
        before_arbitration_hook_for_testing_();
      if (!ring_->TryReleaseCompleteChunkAsFree(position, &observed))
        return Outcome::kRetryLater;
      ++read_pos_;
      return ForwardCopiedChunk(status);
    }

    case ChunkState::kRewriteRequested:
      // The writer still owns this chunk. Resolve the position as a hole.
      ++read_pos_;
      ++num_positions_skipped_;
      return Outcome::kSkipped;

    case ChunkState::kRewriteAcknowledged:
      if (!ring_->TryReleaseRewriteAcknowledgedChunkAsFree(position, &observed))
        return StopOnProtocolError(observed);
      ++read_pos_;
      ++num_positions_skipped_;
      return Outcome::kSkipped;

    case ChunkState::kReserved5:
    case ChunkState::kReserved6:
    case ChunkState::kReserved7:
      // The reader cannot safely reclaim an unknown state.
      return StopOnProtocolError(observed);
  }
}

SharedRingBufferReader::CommittedPrefixStatus
SharedRingBufferReader::CopyCommittedPrefix(uint32_t chunk_index,
                                            uint32_t state_word) {
  copied_fragments_.clear();
  copied_chunk_ = ChunkContents{};
  copied_chunk_.writer_id = WriterIdOf(state_word);
  copied_chunk_.payload_flags = PayloadFlagsOf(state_word);

  const uint32_t num_fragments = NumFragmentsOf(state_word);
  if (num_fragments == 0) {
    // Nothing is published, so there is nothing to copy - and nothing else in
    // the chunk may be touched. In particular the target BufferID is stored
    // while the writer exclusively owns a freshly claimed chunk and is
    // published only by the first release transition out of kBeingWritten, so
    // a BeingWritten word carrying no fragments does not order that store
    // against this reader at all.
    return CommittedPrefixStatus::kNoFragments;
  }

  if (ChunkFormatOf(state_word) != ChunkFormat::kTargetBuffer)
    return CommittedPrefixStatus::kUnsupportedFormat;

  const uint32_t capacity = chunk_size_ - kTargetBufferPayloadOffset;
  const uint8_t* chunk = ring_->chunk_at(chunk_index);
  const uint8_t* const payload_begin = chunk + kTargetBufferPayloadOffset;
  const uint8_t* directory_cursor = chunk + chunk_size_;
  uint32_t total = 0;
  for (uint32_t i = 0; i < num_fragments; ++i) {
    uint32_t fragment_size = 0;
    if (!ReadFragmentSize(payload_begin, &directory_cursor, &fragment_size) ||
        fragment_size > capacity - total) {
      copied_fragments_.clear();
      return CommittedPrefixStatus::kMalformed;
    }
    total += fragment_size;
    copied_fragments_.push_back(Fragment{nullptr, fragment_size});
  }

  const uint32_t directory_bytes =
      static_cast<uint32_t>(chunk + chunk_size_ - directory_cursor);
  if (total > capacity - directory_bytes) {
    copied_fragments_.clear();
    return CommittedPrefixStatus::kMalformed;
  }

  copied_payload_.assign(chunk + kTargetBufferPayloadOffset,
                         chunk + kTargetBufferPayloadOffset + total);
  uint32_t offset = 0;
  for (Fragment& fragment : copied_fragments_) {
    fragment.data = copied_payload_.data() + offset;
    offset += fragment.size;
  }

  copied_chunk_.target_buffer = LoadTargetBufferId(chunk);
  copied_chunk_.fragments = copied_fragments_.data();
  copied_chunk_.num_fragments = num_fragments;
  return CommittedPrefixStatus::kReady;
}

SharedRingBufferReader::Outcome SharedRingBufferReader::ForwardCopiedChunk(
    CommittedPrefixStatus status) {
  switch (status) {
    case CommittedPrefixStatus::kReady:
      ++num_chunks_read_;
      delegate_->OnChunkRead(copied_chunk_);
      return Outcome::kChunkRead;
    case CommittedPrefixStatus::kMalformed:
      ++num_malformed_chunks_;
      delegate_->OnDataLoss(copied_chunk_.writer_id);
      break;
    case CommittedPrefixStatus::kUnsupportedFormat:
      ++num_unknown_format_chunks_;
      delegate_->OnDataLoss(copied_chunk_.writer_id);
      break;
    case CommittedPrefixStatus::kNoFragments:
      break;
  }

  ++num_positions_skipped_;
  return Outcome::kSkipped;
}

SharedRingBufferReader::Outcome SharedRingBufferReader::StopOnProtocolError(
    uint32_t state_word) {
  // Latched, so this logs once per ring rather than once per drain pass.
  has_protocol_error_ = true;
  PERFETTO_ELOG(
      "tracing v2: stopping ring reader at position %u; chunk state word "
      "0x%08x is not something this build can arbitrate",
      read_pos_, state_word);
  return Outcome::kProtocolError;
}

}  // namespace perfetto::tracing_v2
