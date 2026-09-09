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
// Like SharedMemoryABI in v1, this class only interprets memory that somebody
// else owns: the ring buffer's owner supplies the region and keeps it mapped
// for as long as this view, its reader and its writers are around.
//
// The ring buffer is lock-free, with multiple writers and a single reader:
// - Writers reserve positions concurrently. When the ring buffer is full,
//   each writer applies its BufferExhaustedPolicy and records any data it
//   drops.
// - The reader consumes positions in order and is the only actor that makes a
//   chunk Free again.
// - All updates to rw_positions and the chunk state words go through this
//   class.
class SharedRingBuffer {
 public:
  // Attaches to a ring buffer at |start|. Its layout is:
  //
  //   size = sizeof(RingBufferHeader) + num_chunks * chunk_size
  //
  // - num_chunks must be a nonzero power of two, at most 2^30.
  // - |chunk_size| must be at least 256 and a multiple of four. It does not
  //   need to be a power of two.
  // - |size| must match the equation exactly, with no trailing bytes. It does
  //   not need to be a power of two.
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
  // Reserving a position and acquiring its physical chunk are deliberately
  // separate operations. TryReserveWritePos() advances write_pos first.
  // The writer then changes Free(wrap_count(chunk_pos)) to BeingWritten. If it
  // is descheduled between the two, the reader consumes that position as an
  // unclaimed hole and moves the Free word to the next wrap. The delayed
  // writer's exact compare-and-swap then fails, so it cannot publish behind
  // the reader.

  enum class ReserveResult {
    kReserved,
    // num_chunks positions are already outstanding. Nothing was reserved and
    // no hole was created, so a stalling policy may wait and try again.
    kFull,
  };

  struct Reservation {
    ReserveResult result = ReserveResult::kFull;
    // Valid only for kReserved. This is a position in the reservation order,
    // not a physical chunk index.
    uint32_t write_pos = 0;
    // The read_pos sampled by the last reservation attempt. If the writer has
    // to wait, the futex sleeps only while read_pos still has this value.
    uint32_t read_pos_for_wait = 0;
  };

  // Reserves the next position if the ring buffer has room, retrying a lost
  // CAS.
  Reservation TryReserveWritePos();

  // Writer-side chunk transitions.

  // Free(wrap_count(chunk_pos)) -> BeingWritten. |being_written_word| must be a
  // BeingWritten word for this writer with zero fragments.
  //
  // If the compare-and-swap fails, this reservation becomes a hole. Do not
  // retry it against the returned word; reserve a later position.
  bool TryAcquireChunkForWriting(uint32_t chunk_pos,
                                 uint32_t being_written_word);

  // BeingWritten -> Complete. |*expected| is the last BeingWritten word.
  // On failure it receives the current word. It must be RewriteRequested with
  // the same contents; no other actor may change a chunk while this writer owns
  // it.
  bool TryReleaseChunkAsComplete(ChunkIndex chunk_idx,
                                 uint32_t complete_word,
                                 uint32_t* expected);

  // Complete -> BeingWritten, for a writer taking its own cached chunk back to
  // append more fragments. Failure means the reader reclaimed it first; the
  // writer just drops its handle.
  bool TryReacquireChunkForWriting(ChunkIndex chunk_idx, uint32_t observed);

  // RewriteRequested -> RewriteAcknowledged after the writer has stopped
  // touching the old chunk. Failure is a protocol error.
  bool TryAcknowledgeRewrite(ChunkIndex chunk_idx, uint32_t observed);

  // Reader side.

  // Returns the current chunk state word. A writer publishes fragments before
  // changing this word, so the returned word also makes those fragments
  // visible to the reader.
  uint32_t LoadChunkStateWord(ChunkIndex chunk_idx) const;

  // Returns the next reservation position to bound a drain or control barrier.
  // Chunk state determines which reserved positions have published payload.
  uint32_t LoadWritePos() const;

  // BeingWritten -> RewriteRequested, passing format, flags, num_fragments and
  // the WriterID through untouched. |*expected| is the last BeingWritten word;
  // on failure it receives the word that won the race.
  bool TryRequestRewrite(ChunkIndex chunk_idx, uint32_t* expected);

  // The following transitions are the only ones that expose a chunk to the
  // next pass around the ring buffer. The new wrap count comes from
  // |chunk_pos|, not from the old state word.

  // Free(wrap_count(chunk_pos)) -> Free(next_wrap(chunk_pos)). This consumes a
  // position whose writer never entered BeingWritten and prepares the chunk
  // for chunk_pos + num_chunks. |*expected| is the Free word for this position;
  // on failure it receives the current word.
  bool TryMoveFreeChunkToNextWrap(uint32_t chunk_pos, uint32_t* expected);

  // Complete -> Free(next_wrap(chunk_pos)). |*expected| is the last Complete
  // word; on failure it receives the current word.
  bool TryReleaseCompleteChunkAsFree(uint32_t chunk_pos, uint32_t* expected);

  // RewriteAcknowledged -> Free(next_wrap(chunk_pos)). Failure is a protocol
  // error and updates |*observed| with the unexpected word.
  bool TryReleaseRewriteAcknowledgedChunkAsFree(uint32_t chunk_pos,
                                                uint32_t* observed);

  // Backpressure when the ring buffer is full.
  //
  // Writers wait on read_pos; the reader publishes a new value and wakes them.
  // The waiter count only avoids an unnecessary wake syscall. Capacity is
  // always decided from rw_positions.

  enum class WriterWaitResult {
    // The writer must recheck capacity. A wake, a timeout and an interrupted
    // or spurious return all land here: the wait never decides when a writer
    // gives up, the writer's own deadline does.
    kRetry,
    // This build or kernel cannot provide the wait. Do not retry the syscall.
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
