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

#ifndef SRC_TRACING_V2_SHARED_RING_BUFFER_H_
#define SRC_TRACING_V2_SHARED_RING_BUFFER_H_

#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include "perfetto/base/logging.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"

namespace perfetto::tracing_v2 {

namespace test {
class SharedRingBufferInternalsForTest;
}

// Provides the atomic operations on a shared ring buffer.
//
// Like SharedMemoryABI in v1, this class interprets memory from its owner.
// The owner must keep the region mapped until this view, its reader and its
// writers are destroyed.
//
// The ring buffer is lock-free, with multiple writers and a single reader:
// - Writers reserve positions concurrently. When the ring buffer is full,
//   each writer applies its BufferExhaustedPolicy and records any data it
//   drops.
// - The reader consumes positions in order and is the only actor that makes a
//   chunk Free again.
// - All updates to rw_positions and the chunk state words go through this
//   class.
//
// Chunk transitions use compare-and-exchange (CAS):
// - Each CAS compares the entire state word, including all fields.
// - For BeingWritten, Complete and RewriteRequested, those fields are format,
//   flags, num_fragments and WriterID. Free contains the wrap count instead.
// - In transition comments, N and M are published fragment counts.
//
// TODO(sashwinbalaji): CAS operations currently use the default seq_cst order.
// Their implementation comments propose weaker orders for a later optimization
// pass. Audit the synchronization guarantees and measure the benefit before
// replacing seq_cst.
class SharedRingBuffer {
 public:
  // Attaches to a ring buffer at |start|. Its layout is:
  //
  //   size = sizeof(RingBufferHeader) + num_chunks * chunk_size
  //
  // - sizeof(RingBufferHeader) is 64 bytes. |start| must be 64-byte aligned.
  // - num_chunks must be a power of two, from 2 to 2^30.
  // - |chunk_size| must be at least 256 bytes and a multiple of four.
  //   It does not need to be a power of two.
  // - |size| must match the equation exactly, with no trailing bytes.
  //   It does not need to be a power of two.
  //
  // A newly created ring buffer must be zero-filled.
  SharedRingBuffer(uint8_t* start, size_t size, uint32_t chunk_size);
  ~SharedRingBuffer() = default;

  SharedRingBuffer(const SharedRingBuffer&) = delete;
  SharedRingBuffer& operator=(const SharedRingBuffer&) = delete;
  SharedRingBuffer(SharedRingBuffer&&) = delete;
  SharedRingBuffer& operator=(SharedRingBuffer&&) = delete;

  // Immutable for the life of the ring buffer.
  uint32_t num_chunks() const { return num_chunks_; }
  uint32_t chunk_size() const { return chunk_size_; }

  uint8_t* chunk_at(ChunkIndex chunk_idx) {
    PERFETTO_DCHECK(chunk_idx.value() < num_chunks_);
    return start_ + sizeof(RingBufferHeader) +
           static_cast<size_t>(chunk_idx.value()) * chunk_size_;
  }
  const uint8_t* chunk_at(ChunkIndex chunk_idx) const {
    PERFETTO_DCHECK(chunk_idx.value() < num_chunks_);
    return start_ + sizeof(RingBufferHeader) +
           static_cast<size_t>(chunk_idx.value()) * chunk_size_;
  }

  // Writer-side reservation.
  //
  // A writer reserves a position and acquires its physical chunk in separate
  // operations:
  // 1. TryReserveWritePos() advances write_pos.
  // 2. TryAcquireChunkForWriting() changes Free(wrap_count(chunk_pos)) to
  //    BeingWritten.
  //
  // If the reader reaches the reservation before step 2, it must atomically
  // advance the chunk's wrap count before it advances read_pos.
  // The delayed writer's claim then fails because its expected wrap count no
  // longer matches. This prevents publication at a consumed position.

  enum class ReserveResult {
    kReserved,
    // num_chunks positions are already outstanding. Nothing was reserved,
    // so a stalling policy may wait and try again.
    kFull,
  };

  struct Reservation {
    ReserveResult result = ReserveResult::kFull;
    // Valid only for kReserved. This is a position in the reservation order,
    // not a physical chunk index.
    uint32_t chunk_pos = 0;
    // The read_pos sampled by the last reservation attempt. If the writer has
    // to wait, the futex sleeps only while read_pos still has this value.
    uint32_t read_pos_for_wait = 0;
  };

  // Reserves the next position if the ring buffer has room.
  // If another writer or the reader updates rw_positions first, it rechecks
  // capacity with the updated positions before another reservation attempt.
  Reservation TryReserveWritePos();

  // Writer-side chunk transitions.

  // Free(wrap_count(chunk_pos)) -> BeingWritten(0).
  // |being_written_word| supplies this writer's WriterID, format and initial
  // flags. Its fragment count must be zero because the chunk has no published
  // payload for this reservation.
  //
  // On failure, reserve a later position. The reader can consume this position
  // before the claim, or a writer from an earlier reservation can still own
  // the chunk. A retry against the returned word can overwrite that writer's
  // chunk or claim a position the reader already consumed.
  bool TryAcquireChunkForWriting(uint32_t chunk_pos,
                                 uint32_t being_written_word);

  // BeingWritten(N) -> Complete(M). |*expected| is the last BeingWritten word.
  //
  // On failure, |*expected| receives the reader's RewriteRequested(N) word,
  // which must preserve the format, flags, fragment count and WriterID.
  // Only the reader's rewrite request can change the word while this writer
  // owns the chunk. The caller must relocate any unpublished fragment.
  bool TryReleaseChunkAsComplete(ChunkIndex chunk_idx,
                                 uint32_t complete_word,
                                 uint32_t* expected);

  // Complete(N) -> BeingWritten(N), so the writer can append fragments to its
  // cached chunk. |observed| is the Complete word this writer published.
  //
  // On failure, the reader reclaimed the chunk first. The writer must discard
  // its cached handle and acquire another chunk.
  bool TryReacquireChunkForWriting(ChunkIndex chunk_idx, uint32_t observed);

  // RewriteRequested -> RewriteAcknowledged after the writer saves any
  // unpublished fragment and stops access to the old chunk.
  //
  // On failure, report a protocol error. Only this writer can change
  // RewriteRequested, so |observed| must still match the shared word.
  bool TryAcknowledgeRewrite(ChunkIndex chunk_idx, uint32_t observed);

  // Reader side.

  // Returns the current chunk state word with acquire ordering. For a data
  // state, the reader can access the published fragments and their sizes.
  // BufferID is visible only when the published fragment count is nonzero.
  uint32_t LoadChunkStateWordAcquire(ChunkIndex chunk_idx) const;

  // Returns the next logical position a writer can reserve.
  // - This bounds the reservations the reader can consume.
  // - It does not say which chunks have published fragments.
  //   The reader checks each chunk's state word for that.
  // - An older position can shorten a drain pass. A later pass can consume
  //   the remaining reservations.
  uint32_t LoadWritePosRelaxed() const;

  // BeingWritten(N) -> RewriteRequested(N), with all other fields unchanged.
  // |*expected| must be the word the reader used to copy the fragments.
  //
  // On failure, the writer published first and |*expected| receives the current
  // word. The reader must discard its copy and retry this position in Drain().
  bool TryRequestRewrite(ChunkIndex chunk_idx, uint32_t* expected);

  // The following transitions are the only ones that expose a chunk to the
  // next pass around the ring buffer. The new wrap count comes from
  // |chunk_pos|, not from the old state word.

  // Free(wrap_count(chunk_pos)) -> Free(next_wrap(chunk_pos)).
  // Consumes an unclaimed position and prepares the chunk for
  // chunk_pos + num_chunks. |*expected| must be this position's Free word.
  //
  // On failure, the delayed writer claimed the chunk first and |*expected|
  // receives the current word. The reader must retry this position in Drain().
  bool TryMoveFreeChunkToNextWrap(uint32_t chunk_pos, uint32_t* expected);

  // Complete(N) -> Free(next_wrap(chunk_pos)).
  // |*expected| must be the word the reader used to copy the fragments.
  //
  // On failure, the writer reused the chunk first and |*expected| receives the
  // current word. The reader must discard its copy and retry this position in
  // Drain(). If the chunk is now BeingWritten, that retry requests a rewrite.
  bool TryReleaseCompleteChunkAsFree(uint32_t chunk_pos, uint32_t* expected);

  // RewriteAcknowledged -> Free(next_wrap(chunk_pos)).
  //
  // On failure, |*observed| receives the unexpected word. This is a protocol
  // error because only this reader can change an acknowledged chunk.
  bool TryReleaseRewriteAcknowledgedChunkAsFree(uint32_t chunk_pos,
                                                uint32_t* observed);

  // Backpressure: the writer's path when the ring buffer is full.
  //
  // Writers wait on read_pos. The reader publishes a new value and wakes them.
  // The waiter count only avoids an unnecessary wake syscall. Capacity is
  // always decided from rw_positions.

  enum class WriterWaitResult {
    // The writer must recheck capacity after a wake, timeout, interruption or
    // spurious return. The writer's deadline determines when it stops retries.
    kRetry,
    // This build or kernel cannot provide the wait. Do not retry the syscall.
    // The writer can sleep and retry acquisition instead.
    kUnavailable,
  };

  // Whether WaitForReadPosChange() is implemented on this platform.
  static bool SupportsWriterWait();

  // Blocks until read_pos differs from |read_pos_for_wait| or |timeout_ms|
  // elapses. |read_pos_for_wait| comes from the last Reservation, whether it
  // reported kFull or a position whose claim failed.
  WriterWaitResult WaitForReadPosChange(uint32_t read_pos_for_wait,
                                        uint32_t timeout_ms);

  // Publishes read_pos without overwriting a concurrent write_pos update, then
  // wakes waiting writers. Called once per drain pass, so the shared read_pos
  // can lag the reader's local value. The lag only under-reports free capacity.
  void PublishReadPos(uint32_t read_pos);

 private:
  friend class test::SharedRingBufferInternalsForTest;

  // The production entry points load the initial expected value themselves.
  // Tests supply an older value to exercise the CAS retry path
  // deterministically.
  Reservation TryReserveWritePosFromSnapshot(uint64_t rw_positions);
  void PublishReadPosFromSnapshot(uint64_t rw_positions, uint32_t read_pos);

  // Shared-memory address and wrap-count helpers.

  std::atomic<uint32_t>* chunk_state_word_at(ChunkIndex chunk_idx) {
    return reinterpret_cast<std::atomic<uint32_t>*>(chunk_at(chunk_idx));
  }
  const std::atomic<uint32_t>* chunk_state_word_at(ChunkIndex chunk_idx) const {
    return reinterpret_cast<const std::atomic<uint32_t>*>(chunk_at(chunk_idx));
  }

  std::atomic<uint32_t>* chunk_state_word_for_position(uint32_t chunk_pos) {
    return chunk_state_word_at(
        ChunkIndex::FromPosition(chunk_pos, num_chunks_));
  }

  // Returns the Free word for the next position that uses the same chunk.
  uint32_t MakeFreeWordForNextWrap(uint32_t chunk_pos) const {
    // Deriving the value from the next position also handles uint32_t rollover.
    const uint32_t next_pos = chunk_pos + num_chunks_;
    return MakeFreeStateWordForPosition(next_pos, num_chunks_);
  }

  RingBufferHeader* header() {
    return reinterpret_cast<RingBufferHeader*>(start_);
  }
  const RingBufferHeader* header() const {
    return reinterpret_cast<const RingBufferHeader*>(start_);
  }

  uint8_t* const start_;
  const uint32_t num_chunks_;
  const uint32_t chunk_size_;
};

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_V2_SHARED_RING_BUFFER_H_
