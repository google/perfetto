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

// A non-owning view over one ring region, plus the atomic transitions on it.
// Like SharedMemoryABI in v1, this class only interprets memory that somebody
// else owns: the producer allocates the region and keeps it mapped for as
// long as this view, its reader and its writers are around.
//
// SharedRingBufferWriter calls the writer-side methods concurrently.
// SharedRingBufferReader is the only caller of the reader-side methods. This
// is the only class that updates rw_positions or a chunk state word, and only
// the reader writes Free.
class SharedRingBuffer {
 public:
  // Attaches to a ring at |start| without modifying it. |size| covers the
  // header followed by a power-of-two number of |chunk_size|-byte chunks.
  // A newly created ring must be zero-filled.
  SharedRingBuffer(uint8_t* start, size_t size, uint32_t chunk_size);
  ~SharedRingBuffer() = default;

  SharedRingBuffer(const SharedRingBuffer&) = delete;
  SharedRingBuffer& operator=(const SharedRingBuffer&) = delete;
  SharedRingBuffer(SharedRingBuffer&&) = delete;
  SharedRingBuffer& operator=(SharedRingBuffer&&) = delete;

  // Immutable for the life of the ring.
  uint32_t num_chunks() const { return num_chunks_; }
  uint32_t chunk_size() const { return chunk_size_; }

  uint8_t* chunk_at(uint32_t chunk_idx) {
    PERFETTO_DCHECK(chunk_idx < num_chunks_);
    return start_ + sizeof(RingBufferHeader) +
           static_cast<size_t>(chunk_idx) * chunk_size_;
  }
  const uint8_t* chunk_at(uint32_t chunk_idx) const {
    PERFETTO_DCHECK(chunk_idx < num_chunks_);
    return start_ + sizeof(RingBufferHeader) +
           static_cast<size_t>(chunk_idx) * chunk_size_;
  }

  // Writer-side reservation.
  //
  // Reserving a position and acquiring its physical chunk are deliberately
  // separate operations. TryReserveWritePos() advances write_pos first.
  // The writer then changes Free(wrap_count(position)) to BeingWritten. If it
  // is descheduled between the two, the reader resolves that position as an
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
    uint32_t position = 0;
    // The read_pos sampled by the last reservation attempt. If the writer has
    // to wait, the futex sleeps only while read_pos still has this value.
    uint32_t read_pos_for_wait = 0;
  };

  // Reserves the next position if the ring has room. A failed
  // compare-and-swap reserves nothing, and the retry uses the positions that
  // the failed attempt returned.
  Reservation TryReserveWritePos();

  // Writer-side chunk transitions.

  // Free(wrap_count(position)) -> BeingWritten. |being_written_word| must be an
  // BeingWritten word for this writer with zero fragments.
  //
  // If the compare-and-swap fails, this reservation becomes a hole. Do not
  // retry it against the returned word; reserve a later position.
  bool TryAcquireChunkForWriting(uint32_t position,
                                 uint32_t being_written_word);

  // BeingWritten -> Complete. On failure, |*observed| receives the current
  // word. It must be RewriteRequested with the same contents; no other actor
  // may change a chunk while this writer owns it.
  bool TryReleaseChunkAsComplete(uint32_t chunk_idx,
                                 uint32_t* observed,
                                 uint32_t complete_word);

  // Complete -> BeingWritten, for a writer taking its own cached chunk back to
  // append more fragments. Failure means the reader reclaimed it first; the
  // writer just drops its handle.
  bool TryReacquireChunkForWriting(uint32_t chunk_idx, uint32_t observed);

  // RewriteRequested -> RewriteAcknowledged after the writer has stopped
  // touching the old chunk. Failure is a protocol error.
  bool TryAcknowledgeRewrite(uint32_t chunk_idx, uint32_t observed);

  // Reader side.

  // Returns the current chunk state word. A writer publishes fragments before
  // changing this word, so the returned word also makes those fragments
  // visible to the reader.
  uint32_t LoadChunkStateWord(uint32_t chunk_idx) const;

  // Loads write_pos once. A writer may reserve the next position concurrently.
  // The reader will see it on its next pass.
  uint32_t LoadWritePos() const;

  // BeingWritten -> RewriteRequested, passing format, flags, num_fragments and
  // the WriterID through untouched. On failure |*observed| receives the word
  // that won the race.
  bool TryRequestRewrite(uint32_t chunk_idx, uint32_t* observed);

  // The following transitions are the only ones that expose a chunk to the
  // next pass around the ring. The new wrap count comes from |position|, not
  // from the old state word.

  // Free(wrap_count(position)) -> Free(next_wrap(position)). This consumes a
  // position whose writer never entered BeingWritten and prepares the chunk
  // for position + num_chunks.
  bool TryMoveFreeChunkToNextWrap(uint32_t position, uint32_t* observed);

  // Complete -> Free(next_wrap(position)).
  bool TryReleaseCompleteChunkAsFree(uint32_t position, uint32_t* observed);

  // RewriteAcknowledged -> Free(next_wrap(position)). Failure is a protocol
  // error and updates |*observed| with the unexpected word.
  bool TryReleaseRewriteAcknowledgedChunkAsFree(uint32_t position,
                                                uint32_t* observed);

  // Backpressure: the writer's full-ring path.
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

  std::atomic<uint32_t>* chunk_state_word_at(uint32_t chunk_idx) {
    return reinterpret_cast<std::atomic<uint32_t>*>(chunk_at(chunk_idx));
  }
  const std::atomic<uint32_t>* chunk_state_word_at(uint32_t chunk_idx) const {
    return reinterpret_cast<const std::atomic<uint32_t>*>(chunk_at(chunk_idx));
  }

  // Returns the Free word for the next position that uses the same chunk.
  uint32_t MakeFreeWordForNextWrap(uint32_t position) const {
    // Deriving the value from the next position also handles uint32_t rollover.
    return MakeFreeStateWord(
        WrapCountForPosition(position + num_chunks_, num_chunks_));
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
