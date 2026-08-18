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

#include "src/tracing/v2/chunk_reader.h"

#include <stdint.h>

#include <utility>

#include "perfetto/base/logging.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/tracing_v2_abi.h"

namespace perfetto::tracing_v2 {
namespace {

// How many times one call to ReadOne() may redispatch on a word a failed
// compare-and-swap handed back. The budget only goes down while a writer keeps
// winning a publication or reuse race, which means somebody is making progress;
// a writer sitting quietly in Acquired is marked on the first look.
constexpr uint32_t kContentionBudget = 8;

}  // namespace

ChunkReader::Delegate::~Delegate() = default;

ChunkReader::ChunkReader(SharedRingBuffer* ring, Delegate* delegate)
    : ring_(ring),
      delegate_(delegate),
      num_chunks_(ring->num_chunks()),
      chunk_size_(ring->chunk_size()),
      chunk_bits_(ring->chunk_bits()),
      fragment_size_width_(ring->fragment_size_width()) {
  payload_scratch_.reserve(chunk_size_);
  directory_scratch_.reserve(kMaxFragmentsPerChunk * fragment_size_width_);
  fragments_.reserve(kMaxFragmentsPerChunk);
}

ChunkReader::~ChunkReader() = default;

ChunkReader::ReadOutcome ChunkReader::ReadOne() {
  if (stopped_)
    return ReadOutcome::kProtocolError;

  // One relaxed snapshot, and every cursor decision below is taken from it. A
  // stale value can only make this pass resolve fewer positions than exist.
  const uint32_t write_pos = ring_->LoadWritePosRelaxed();
  const uint32_t outstanding = PositionDistance(write_pos, read_pos_);
  if (outstanding == 0)
    return ReadOutcome::kNoData;
  if (outstanding > num_chunks_) {
    // A writer only ever reserves a position after checking that fewer than
    // num_chunks are outstanding, so no legal producer can put the cursors this
    // far apart. Today that means the ring is corrupt; once the mapping is
    // writable by another process it also means a producer trying to keep the
    // reader busy resolving positions that do not exist.
    stopped_ = true;
    PERFETTO_ELOG(
        "tracing v2: stopping ring reader; write_pos %u is %u positions ahead "
        "of read_pos %u, which is more than the %u chunks in the ring",
        write_pos, outstanding, read_pos_, num_chunks_);
    return ReadOutcome::kProtocolError;
  }

  const uint32_t position = read_pos_;
  const uint32_t chunk_index = ChunkIndexOfPosition(position, num_chunks_);

  // One acquire load, then every decision below is taken against this snapshot.
  // A failed compare-and-swap replaces it with the word the reader is now
  // entitled to decide on, and nothing re-reads the chunk behind its back.
  uint32_t observed = ring_->LoadStateAcquire(chunk_index);

  for (uint32_t budget = kContentionBudget; budget > 0; --budget) {
    switch (StateOf(observed)) {
      case ChunkState::kFreeForWrap: {
        if (WrapCountOf(observed) !=
            WrapCountOfPosition(position, chunk_bits_)) {
          // No legal transition puts another traversal's wrap count under the
          // reader: only the reader writes one, and it derives it from the
          // position it is resolving. So this is corruption, an incompatible
          // ABI, or a mapping whose previous reader died between the chunk
          // transition and its cursor store.
          return Stop(observed);
        }
        // Nobody claimed this position. One compare-and-swap consumes the hole
        // and hands the chunk to the writer holding position + num_chunks.
        if (ring_->TryAdvanceUnclaimed(position, &observed)) {
          ++read_pos_;
          ++num_holes_;
          return ReadOutcome::kSkipped;
        }
        continue;
      }

      case ChunkState::kAcquired: {
        // Copy first, arbitrate second. The copy is speculative until the
        // compare-and-swap below succeeds, because only that linearises it
        // against the writer.
        const bool usable = SnapshotCommittedPrefix(chunk_index, observed);
        if (before_arbitration_hook_for_testing_)
          before_arbitration_hook_for_testing_();
        // Marking is unconditional. Whether the bytes are usable decides if
        // anything is emitted; it does not decide whether the old writer has to
        // be stopped from publishing behind us.
        if (!ring_->TryMarkForRewrite(chunk_index, &observed))
          continue;  // The writer published more. Discard the copy, redispatch.
        ++read_pos_;
        ++num_scrapes_;
        return EmitSnapshot(usable);
      }

      case ChunkState::kComplete: {
        const bool usable = SnapshotCommittedPrefix(chunk_index, observed);
        if (before_arbitration_hook_for_testing_)
          before_arbitration_hook_for_testing_();
        if (!ring_->TryReclaimComplete(position, &observed))
          continue;  // The writer reused it. Discard the copy, redispatch.
        ++read_pos_;
        return EmitSnapshot(usable);
      }

      case ChunkState::kRewriteRequested:
        // Only the owning writer may leave this state, so the reader resolves
        // the position as a hole and moves on. This is what keeps a stalled
        // writer from blocking anything but its own chunk.
        ++read_pos_;
        ++num_holes_;
        return ReadOutcome::kSkipped;

      case ChunkState::kAcknowledged:
        if (!ring_->TryReclaimAcknowledged(position))
          return Stop(observed);
        ++read_pos_;
        ++num_holes_;
        return ReadOutcome::kSkipped;

      case ChunkState::kReserved5:
      case ChunkState::kReserved6:
      case ChunkState::kReserved7:
        // We do not know who owns this chunk or which transition releases it,
        // and guessing could hand a stale writer a position behind the cursor.
        return Stop(observed);
    }
  }

  // A writer kept winning publication or reuse races for a whole budget, which
  // means somebody is making progress. Leave read_pos alone and come back.
  return ReadOutcome::kRetryLater;
}

ChunkReader::DrainResult ChunkReader::Drain(uint32_t max_positions) {
  const uint32_t start_pos = read_pos_;
  DrainResult result{};
  for (uint32_t i = 0; i < max_positions; ++i) {
    result.last_outcome = ReadOne();
    if (result.last_outcome != ReadOutcome::kEmitted &&
        result.last_outcome != ReadOutcome::kSkipped) {
      break;
    }
  }

  result.positions_resolved = PositionDistance(read_pos_, start_pos);
  if (result.positions_resolved != 0) {
    // One cursor store and at most one wake for the whole pass, rather than one
    // per reclaimed chunk.
    ring_->PublishReadPos(read_pos_);
  }
  return result;
}

bool ChunkReader::SnapshotCommittedPrefix(uint32_t chunk_index,
                                          uint32_t state_word) {
  fragments_.clear();
  pending_contents_ = ChunkContents{};
  pending_contents_.writer_id = WriterIdOf(state_word);
  pending_contents_.payload_flags = PayloadFlagsOf(state_word);

  const uint32_t num_fragments = NumFragmentsOf(state_word);
  if (num_fragments == 0) {
    // Nothing is published, so there is nothing to copy - and nothing else in
    // the chunk may be touched. In particular the target BufferID is stored
    // while the writer exclusively owns a freshly claimed chunk and is
    // published only by the first release transition out of kAcquired, so an
    // Acquired word carrying no fragments does not order that store against
    // this reader at all.
    return false;
  }

  // TODO(sashwinbalaji): this counter and num_malformed_chunks_ below are the
  // two places where the reader silently discards committed payload. The
  // traced-side reader must attribute both to the producer that owns the ring
  // and expose them through trace stats, the way the service already accounts
  // for v1 chunk loss; in-process they are only reachable from the accessors.
  if (FormatOf(state_word) != ChunkFormat::kTargetBuffer) {
    // We still know who owns the chunk, because that is all in the state, so
    // the ownership transition happens as usual. We must not touch the payload
    // or any format-specific header field, including the BufferID.
    ++num_unknown_format_chunks_;
    return false;
  }

  const uint32_t width = fragment_size_width_;
  const uint32_t capacity = chunk_size_ - kTargetBufferPayloadOffset;
  // num_fragments is at most 255 and width at most 2, so this is at most 510
  // and cannot overflow.
  const uint32_t directory_bytes = num_fragments * width;
  if (directory_bytes > capacity) {
    ++num_malformed_chunks_;
    return false;
  }

  const uint8_t* chunk = ring_->chunk_at(chunk_index);
  const uint32_t directory_start = chunk_size_ - directory_bytes;

  // Copy the directory before parsing it, so every boundary comes from one
  // private snapshot instead of from repeated reads of memory a writer still
  // owns. The sizes remain producer claims: the checks below make the walk
  // memory-safe, they do not make its contents true.
  directory_scratch_.assign(chunk + directory_start, chunk + chunk_size_);

  const uint32_t payload_capacity = capacity - directory_bytes;
  uint32_t total = 0;
  for (uint32_t i = 0; i < num_fragments; ++i) {
    const uint32_t entry_offset =
        FragmentSizeEntryOffset(chunk_size_, width, i);
    const uint32_t size = LoadFragmentSize(
        directory_scratch_.data() + (entry_offset - directory_start), width);
    // Each size is at most 65535 and there are at most 255 of them, so the
    // running sum stays far inside a uint32_t; the bound below is what makes
    // the walk safe.
    total += size;
    if (total > payload_capacity) {
      ++num_malformed_chunks_;
      fragments_.clear();
      return false;
    }
    fragments_.push_back(Fragment{nullptr, size});
  }

  payload_scratch_.assign(chunk + kTargetBufferPayloadOffset,
                          chunk + kTargetBufferPayloadOffset + total);
  uint32_t offset = 0;
  for (Fragment& fragment : fragments_) {
    fragment.data = payload_scratch_.data() + offset;
    offset += fragment.size;
  }

  pending_contents_.target_buffer = LoadTargetBuffer(chunk);
  pending_contents_.fragments = fragments_.data();
  pending_contents_.num_fragments = num_fragments;
  return true;
}

ChunkReader::ReadOutcome ChunkReader::EmitSnapshot(bool snapshot_usable) {
  if (!snapshot_usable || pending_contents_.num_fragments == 0) {
    ++num_holes_;
    return ReadOutcome::kSkipped;
  }
  ++num_chunks_emitted_;
  delegate_->OnChunkRead(pending_contents_);
  return ReadOutcome::kEmitted;
}

ChunkReader::ReadOutcome ChunkReader::Stop(uint32_t state_word) {
  // Latched, so this logs once per ring rather than once per drain pass.
  stopped_ = true;
  PERFETTO_ELOG(
      "tracing v2: stopping ring reader at position %u; chunk state word "
      "0x%08x is not something this build can arbitrate",
      read_pos_, state_word);
  return ReadOutcome::kProtocolError;
}

}  // namespace perfetto::tracing_v2
