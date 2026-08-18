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
#include <memory>
#include <optional>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/paged_memory.h"
#include "src/tracing/v2/tracing_v2_abi.h"

namespace perfetto::tracing_v2 {

// Owns the producer-local ring and implements its atomic transitions.
//
// SharedRingBufferWriter calls the writer-side methods concurrently.
// SharedRingBufferReader is the only caller of the reader-side methods. This
// is the only class that updates rw_positions or a chunk state word, and only
// the reader writes Free.
class SharedRingBuffer {
 public:
  // |num_chunks| must be a power of two in [1, 2^30]. |chunk_size| must be at
  // least 256 bytes and a multiple of four. Returns nullptr if the parameters
  // are invalid, the allocation size overflows, or the allocation fails.
  static std::unique_ptr<SharedRingBuffer> Create(uint32_t num_chunks,
                                                  uint32_t chunk_size);

  ~SharedRingBuffer();

  SharedRingBuffer(const SharedRingBuffer&) = delete;
  SharedRingBuffer& operator=(const SharedRingBuffer&) = delete;
  SharedRingBuffer(SharedRingBuffer&&) = delete;
  SharedRingBuffer& operator=(SharedRingBuffer&&) = delete;

  // Immutable for the life of the ring.
  uint32_t num_chunks() const { return num_chunks_; }
  uint32_t chunk_size() const { return chunk_size_; }
  uint32_t chunk_index_bits() const { return chunk_index_bits_; }

  uint8_t* chunk_at(uint32_t chunk_index) {
    PERFETTO_DCHECK(chunk_index < num_chunks_);
    return chunks_begin_ + static_cast<size_t>(chunk_index) * chunk_size_;
  }
  const uint8_t* chunk_at(uint32_t chunk_index) const {
    PERFETTO_DCHECK(chunk_index < num_chunks_);
    return chunks_begin_ + static_cast<size_t>(chunk_index) * chunk_size_;
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
    // no hole was created; a stalling policy may wait and try again.
    kFull,
  };

  struct Reservation {
    ReserveResult result = ReserveResult::kFull;
    // Valid only for kReserved. This is a position in the reservation order,
    // not a physical chunk index.
    uint32_t position = 0;
    // The read_pos used for the capacity check. A writer that receives kFull
    // passes this to WaitForReadPosChange() to avoid a lost wakeup.
    uint32_t read_pos_sample = 0;
  };

  // Reserves the next position if the ring has room. A failed
  // compare-and-swap did not reserve anything, so the operation retries with
  // the positions returned by the compare-and-swap.
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
  bool TrySetChunkComplete(uint32_t chunk_index,
                           uint32_t* observed,
                           uint32_t complete_word);

  // Complete -> BeingWritten, for a writer taking its own cached chunk back to
  // append more fragments. Failure means the reader reclaimed it first; the
  // writer just drops its handle.
  bool TryReacquireChunkForWriting(uint32_t chunk_index, uint32_t observed);

  // RewriteRequested -> RewriteAcknowledged after the writer has stopped
  // touching the old chunk. Failure is a protocol error.
  bool TryAcknowledgeRewrite(uint32_t chunk_index, uint32_t observed);

  // Reader side.

  // Returns the current chunk state word. A writer publishes fragments before
  // changing this word, so the returned word also makes those fragments
  // visible to the reader.
  uint32_t LoadChunkStateWord(uint32_t chunk_index) const;

  // Loads write_pos once. A writer may reserve the next position concurrently;
  // the reader will see it on its next pass.
  uint32_t LoadWritePos() const;

  // BeingWritten -> RewriteRequested, passing format, flags, num_fragments and
  // the WriterID through untouched. On failure |*observed| receives the word
  // that won the race.
  bool TryRequestRewrite(uint32_t chunk_index, uint32_t* observed);

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
    // The writer must recheck capacity. This also covers interrupted and
    // spurious wakes.
    kRetry,
    kTimedOut,
    // This build or kernel cannot provide the wait. Do not retry the syscall.
    kUnavailable,
  };

  // Whether WaitForReadPosChange() is implemented on this platform.
  static bool SupportsWriterWait();

  // Blocks until read_pos changes or |timeout_ms| elapses.
  // |expected_read_pos| must be the sample used for the kFull result.
  WriterWaitResult WaitForReadPosChange(uint32_t expected_read_pos,
                                        uint32_t timeout_ms);

  // Publishes read_pos without overwriting a concurrent write_pos update, then
  // wakes waiting writers. Called once per drain pass, so the shared read_pos
  // can lag the reader's local value; it only under-reports free capacity.
  void PublishReadPos(uint32_t read_pos);

  // Testing.

  // Exposes the allocation-size calculation for overflow tests.
  static std::optional<size_t> ComputeAllocationSizeForTesting(
      uint32_t num_chunks,
      uint32_t chunk_size,
      size_t page_size);

  // Exposes the futex error policy without requiring a failing syscall.
  static WriterWaitResult ClassifyWriterWaitErrnoForTesting(int wait_errno);

  // Diagnostics only. Never a correctness input.
  uint32_t num_writers_waiting_for_testing() const;
  uint32_t read_pos_for_testing() const;

  // Starts the production CAS loops with |initial_rw_positions|. Tests pass an
  // old value to force the first compare-and-swap to fail.
  Reservation TryReserveWritePosForTesting(uint64_t initial_rw_positions);
  void PublishReadPosForTesting(uint64_t initial_rw_positions,
                                uint32_t read_pos);

  // Injects a state word for corruption and unknown-ABI tests.
  void SetStateWordForTesting(uint32_t chunk_index, uint32_t state_word);

  // Injects a write_pos without reserving the intervening positions.
  void SetWritePosForTesting(uint32_t write_pos);

  // Seeds a valid ring state near a position or wrap-count rollover.
  void SetPositionsForTesting(uint32_t position);

 private:
  SharedRingBuffer(base::PagedMemory ring_memory,
                   uint32_t num_chunks,
                   uint32_t chunk_size);

  // CAS loops shared by the production entry points and deterministic race
  // tests. Production starts them with a freshly loaded rw_positions value;
  // tests can supply an older value to force the first CAS to fail.
  Reservation TryReserveWritePosImpl(uint64_t expected_rw_positions);
  void PublishReadPosImpl(uint64_t expected_rw_positions, uint32_t read_pos);

  // Shared-memory address and wrap-count helpers.

  std::atomic<uint32_t>* chunk_state_word_at(uint32_t chunk_index) {
    return reinterpret_cast<std::atomic<uint32_t>*>(chunk_at(chunk_index));
  }
  const std::atomic<uint32_t>* chunk_state_word_at(uint32_t chunk_index) const {
    return reinterpret_cast<const std::atomic<uint32_t>*>(
        chunk_at(chunk_index));
  }

  // Returns the Free word for the next position that uses the same chunk.
  uint32_t MakeFreeWordForNextWrap(uint32_t position) const {
    // Deriving the value from the next position also handles uint32_t rollover.
    return MakeFreeStateWord(
        WrapCountForPosition(position + num_chunks_, chunk_index_bits_));
  }

  RingBufferHeader* header() {
    return static_cast<RingBufferHeader*>(ring_memory_.Get());
  }
  const RingBufferHeader* header() const {
    return static_cast<const RingBufferHeader*>(ring_memory_.Get());
  }

  base::PagedMemory ring_memory_;
  uint8_t* const chunks_begin_;
  const uint32_t num_chunks_;
  const uint32_t chunk_size_;
  const uint32_t chunk_index_bits_;
};

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_V2_SHARED_RING_BUFFER_H_
