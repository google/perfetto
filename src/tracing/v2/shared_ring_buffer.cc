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

#include "src/tracing/v2/shared_ring_buffer.h"

#include <errno.h>
#include <stdint.h>

#include <atomic>
#include <memory>
#include <optional>
#include <utility>

#include "perfetto/base/build_config.h"
#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/paged_memory.h"
#include "perfetto/ext/base/utils.h"
#include "src/tracing/v2/tracing_v2_abi.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX_BUT_NOT_QNX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#define PERFETTO_TRACING_V2_HAS_FUTEX() 1
#else
#define PERFETTO_TRACING_V2_HAS_FUTEX() 0
#endif

#if PERFETTO_TRACING_V2_HAS_FUTEX()
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif

namespace perfetto::tracing_v2 {
namespace {

// PagedMemory adds two guard pages and rounds the mapping up to a page.
std::optional<size_t> ComputeAllocationSize(uint32_t num_chunks,
                                            uint32_t chunk_size,
                                            size_t page_size) {
  const uint64_t ring_size = uint64_t{sizeof(RingBufferHeader)} +
                             uint64_t{num_chunks} * uint64_t{chunk_size};
  if (ring_size > SIZE_MAX || !base::IsPowerOfTwo(page_size))
    return std::nullopt;

  const size_t allocation_size = static_cast<size_t>(ring_size);

  // PagedMemory rounds the request up and adds two guard pages using unchecked
  // size_t arithmetic. Reject sizes that would overflow either operation.
  const size_t page_rounding = page_size - 1;
  if (allocation_size > SIZE_MAX - page_rounding)
    return std::nullopt;
  const size_t rounded_allocation_size =
      (allocation_size + page_rounding) & ~page_rounding;

  if (page_size > SIZE_MAX / 2)
    return std::nullopt;
  const size_t guard_pages_size = page_size * 2;
  if (rounded_allocation_size > SIZE_MAX - guard_pages_size)
    return std::nullopt;

  return allocation_size;
}

#if PERFETTO_TRACING_V2_HAS_FUTEX()
// A futex operation acts on one aligned 32-bit word. The word writers park on
// is the low half of rw_positions. This pointer is passed only to the kernel;
// C++ accesses the field as atomic<uint64_t>.
uint32_t* ReadPosFutexWord(std::atomic<uint64_t>* rw_positions) {
  static_assert(PERFETTO_IS_LITTLE_ENDIAN(),
                "The low-word futex requires read_pos to be the first four "
                "bytes of rw_positions");
  return reinterpret_cast<uint32_t*>(rw_positions);
}

// TODO(sashwinbalaji): Drop *_PRIVATE when the reader moves into traced.
// Private futexes are keyed by process, so a traced wake would not match a
// producer wait.
int FutexSyscall(uint32_t* word,
                 int op,
                 uint32_t value,
                 const struct timespec* timeout) {
  return static_cast<int>(
      syscall(SYS_futex, word, op, value, timeout, nullptr, 0));
}
#endif  // PERFETTO_TRACING_V2_HAS_FUTEX()

SharedRingBuffer::WriterWaitResult ClassifyWriterWaitErrno(int wait_errno) {
  switch (wait_errno) {
    case ETIMEDOUT:
      return SharedRingBuffer::WriterWaitResult::kTimedOut;
    case EAGAIN:
    case EINTR:
      return SharedRingBuffer::WriterWaitResult::kRetry;
    default:
      return SharedRingBuffer::WriterWaitResult::kUnavailable;
  }
}

}  // namespace

// --- Allocation and lifetime. ---

// static
std::unique_ptr<SharedRingBuffer> SharedRingBuffer::Create(
    uint32_t num_chunks,
    uint32_t chunk_size) {
  if (!base::IsPowerOfTwo(num_chunks) || num_chunks > kMaxChunksPerRing ||
      chunk_size < kMinChunkSize || chunk_size % kChunkAlignmentBytes != 0) {
    return nullptr;
  }

  const std::optional<size_t> allocation_size =
      ComputeAllocationSize(num_chunks, chunk_size, base::GetSysPageSize());
  if (!allocation_size)
    return nullptr;

  // PagedMemory is zero-filled. Zero is both (write_pos=0, read_pos=0) and
  // Free(0), so no per-chunk initialization pass is needed.
  auto ring_memory = base::PagedMemory::Allocate(*allocation_size,
                                                 base::PagedMemory::kMayFail);
  if (!ring_memory.IsValid())
    return nullptr;

  return std::unique_ptr<SharedRingBuffer>(
      new SharedRingBuffer(std::move(ring_memory), num_chunks, chunk_size));
}

SharedRingBuffer::SharedRingBuffer(base::PagedMemory ring_memory,
                                   uint32_t num_chunks,
                                   uint32_t chunk_size)
    : ring_memory_(std::move(ring_memory)),
      chunks_begin_(static_cast<uint8_t*>(ring_memory_.Get()) +
                    sizeof(RingBufferHeader)),
      num_chunks_(num_chunks),
      chunk_size_(chunk_size),
      chunk_index_bits_(GetChunkIndexBits(num_chunks)) {}

SharedRingBuffer::~SharedRingBuffer() = default;

// --- Writer-side reservation. ---

SharedRingBuffer::Reservation SharedRingBuffer::TryReserveWritePos() {
  // PublishReadPos() releases after reclaiming chunks. If this load sees a new
  // read_pos, acquire also sees those reclaims. If the load is stale, the
  // compare-and-swap below fails unless the complete rw_positions word is
  // still current.
  return TryReserveWritePosImpl(
      header()->rw_positions.load(std::memory_order_acquire));
}

SharedRingBuffer::Reservation SharedRingBuffer::TryReserveWritePosImpl(
    uint64_t expected_rw_positions) {
  RingBufferHeader* ring_header = header();
  Reservation reservation{};
  for (;;) {
    const uint32_t write_pos = WritePosOf(expected_rw_positions);
    const uint32_t read_pos = ReadPosOf(expected_rw_positions);
    reservation.read_pos_sample = read_pos;

    if (NumOutstandingPositions(write_pos, read_pos) >= num_chunks_) {
      reservation.result = ReserveResult::kFull;
      return reservation;
    }

    // Acquire pairs with the reader publishing newly reclaimed capacity.
    if (ring_header->rw_positions.compare_exchange_weak(
            expected_rw_positions, PackRwPositions(write_pos + 1, read_pos),
            std::memory_order_acquire, std::memory_order_acquire)) {
      reservation.result = ReserveResult::kReserved;
      reservation.position = write_pos;
      return reservation;
    }
    // Failure updates |expected_rw_positions|. No reservation, and therefore
    // no hole, was created.
  }
}

// --- Writer-side chunk transitions. ---

bool SharedRingBuffer::TryAcquireChunkForWriting(uint32_t position,
                                                 uint32_t being_written_word) {
  PERFETTO_DCHECK(ChunkStateOf(being_written_word) ==
                  ChunkState::kBeingWritten);
  PERFETTO_DCHECK(NumFragmentsOf(being_written_word) == 0);

  // A reservation authorizes a claim against this exact Free word.
  uint32_t expected =
      MakeFreeStateWord(WrapCountForPosition(position, chunk_index_bits_));

  // Acquire on success consumes the reader's release reclaim, so the reader's
  // last reads of the previous traversal's payload happen before this writer's
  // first store into the chunk. On failure this position is left unclaimed;
  // the caller does not use the word returned in |expected|.
  return chunk_state_word_at(ChunkIndexOfPosition(position, num_chunks_))
      ->compare_exchange_strong(expected, being_written_word,
                                std::memory_order_acquire,
                                std::memory_order_relaxed);
}

bool SharedRingBuffer::TrySetChunkComplete(uint32_t chunk_index,
                                           uint32_t* observed,
                                           uint32_t complete_word) {
  PERFETTO_DCHECK(ChunkStateOf(*observed) == ChunkState::kBeingWritten);
  PERFETTO_DCHECK(ChunkStateOf(complete_word) == ChunkState::kComplete);

  // Release publishes the new fragments, their size varints and, on the first
  // publication, the target BufferID. On failure the reader has marked the
  // chunk, and acquire orders the reader's completed copy ahead of this
  // writer's relocation work.
  return chunk_state_word_at(chunk_index)
      ->compare_exchange_strong(*observed, complete_word,
                                std::memory_order_release,
                                std::memory_order_acquire);
}

bool SharedRingBuffer::TryReacquireChunkForWriting(uint32_t chunk_index,
                                                   uint32_t observed) {
  PERFETTO_DCHECK(ChunkStateOf(observed) == ChunkState::kComplete);

  uint32_t expected = observed;
  // This relaxed read-modify-write remains in the release sequence started by
  // TrySetChunkComplete(). A reader that acquire-loads the resulting
  // BeingWritten word therefore sees the prefix already published by this
  // writer. On failure the writer drops its cached handle and does not use the
  // returned state.
  return chunk_state_word_at(chunk_index)
      ->compare_exchange_strong(
          expected, ReplaceChunkState(observed, ChunkState::kBeingWritten),
          std::memory_order_relaxed, std::memory_order_relaxed);
}

bool SharedRingBuffer::TryAcknowledgeRewrite(uint32_t chunk_index,
                                             uint32_t observed) {
  PERFETTO_DCHECK(ChunkStateOf(observed) == ChunkState::kRewriteRequested);

  uint32_t expected = observed;
  // Release tells the reader that the writer has finished all accesses to the
  // old chunk. Only this writer may leave RewriteRequested, so failure is a
  // protocol error.
  return chunk_state_word_at(chunk_index)
      ->compare_exchange_strong(expected, kRewriteAcknowledgedStateWord,
                                std::memory_order_release,
                                std::memory_order_relaxed);
}

// --- Reader-side chunk transitions. ---

uint32_t SharedRingBuffer::LoadChunkStateWord(uint32_t chunk_index) const {
  // Pairs with every writer release transition, so each published fragment and
  // its size varint is visible before the reader walks and copies them.
  return chunk_state_word_at(chunk_index)->load(std::memory_order_acquire);
}

uint32_t SharedRingBuffer::LoadWritePos() const {
  // Once a position is observed, the chunk word provides payload visibility.
  return WritePosOf(header()->rw_positions.load(std::memory_order_relaxed));
}

bool SharedRingBuffer::TryRequestRewrite(uint32_t chunk_index,
                                         uint32_t* observed) {
  PERFETTO_DCHECK(ChunkStateOf(*observed) == ChunkState::kBeingWritten);

  // On success, release finishes the reader's copy before the writer sees the
  // rewrite request and relocates its suffix. On failure, the reader discards
  // its copy and retries the position with a new acquire load.
  return chunk_state_word_at(chunk_index)
      ->compare_exchange_strong(
          *observed,
          ReplaceChunkState(*observed, ChunkState::kRewriteRequested),
          std::memory_order_release, std::memory_order_relaxed);
}

bool SharedRingBuffer::TryMoveFreeChunkToNextWrap(uint32_t position,
                                                  uint32_t* observed) {
  PERFETTO_DCHECK(ChunkStateOf(*observed) == ChunkState::kFree);

  // Release finishes the reader's accesses before the next writer claims the
  // chunk. On failure, the reader retries the position with a new acquire
  // load.
  return chunk_state_word_at(ChunkIndexOfPosition(position, num_chunks_))
      ->compare_exchange_strong(*observed, MakeFreeWordForNextWrap(position),
                                std::memory_order_release,
                                std::memory_order_relaxed);
}

bool SharedRingBuffer::TryReleaseCompleteChunkAsFree(uint32_t position,
                                                     uint32_t* observed) {
  PERFETTO_DCHECK(ChunkStateOf(*observed) == ChunkState::kComplete);

  // Release finishes the reader's copy before the next writer uses the chunk.
  // On failure, the reader discards its copy and retries the position with a
  // new acquire load.
  return chunk_state_word_at(ChunkIndexOfPosition(position, num_chunks_))
      ->compare_exchange_strong(*observed, MakeFreeWordForNextWrap(position),
                                std::memory_order_release,
                                std::memory_order_relaxed);
}

bool SharedRingBuffer::TryReleaseRewriteAcknowledgedChunkAsFree(
    uint32_t position,
    uint32_t* observed) {
  // RewriteAcknowledged has one canonical encoding. Compare against that exact
  // word rather than trusting a value that merely decodes to the same state.
  uint32_t expected = kRewriteAcknowledgedStateWord;
  // Acquire observes the writer's final release. Release hands the chunk to the
  // next writer. Only the reader may leave RewriteAcknowledged, so failure is a
  // protocol error.
  const bool reclaimed =
      chunk_state_word_at(ChunkIndexOfPosition(position, num_chunks_))
          ->compare_exchange_strong(expected, MakeFreeWordForNextWrap(position),
                                    std::memory_order_acq_rel,
                                    std::memory_order_relaxed);
  if (!reclaimed)
    *observed = expected;
  return reclaimed;
}

// --- Backpressure. ---

// static
bool SharedRingBuffer::SupportsWriterWait() {
  return PERFETTO_TRACING_V2_HAS_FUTEX();
}

SharedRingBuffer::WriterWaitResult SharedRingBuffer::WaitForReadPosChange(
    uint32_t expected_read_pos,
    uint32_t timeout_ms) {
  PERFETTO_DCHECK(timeout_ms > 0);
#if !PERFETTO_TRACING_V2_HAS_FUTEX()
  base::ignore_result(expected_read_pos);
  base::ignore_result(timeout_ms);
  return WriterWaitResult::kUnavailable;
#else
  RingBufferHeader* ring_header = header();

  // num_writers_waiting and rw_positions are separate atomics. Without the two
  // seq_cst fences, this execution would be possible:
  //
  //   writer                              reader
  //   ------                              ------
  //   increment num_writers_waiting       publish read_pos
  //   read the old read_pos               read zero waiters
  //
  // The writer would sleep after the reader skipped the wake. The paired
  // fences forbid both loads from missing the other side's store. The waiter
  // count carries no data, so the operations around the fences stay relaxed.
  ring_header->num_writers_waiting.fetch_add(1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_seq_cst);

  WriterWaitResult result = WriterWaitResult::kRetry;
  if (ReadPosOf(ring_header->rw_positions.load(std::memory_order_relaxed)) ==
      expected_read_pos) {
    struct timespec timeout{};
    timeout.tv_sec = static_cast<time_t>(timeout_ms / 1000);
    timeout.tv_nsec = static_cast<long>((timeout_ms % 1000) * 1000000);

    // FUTEX_WAIT checks the value again before sleeping. If read_pos changed
    // after the load above, the syscall returns EAGAIN and no wake is lost.
    const int futex_result =
        FutexSyscall(ReadPosFutexWord(&ring_header->rw_positions),
                     FUTEX_WAIT_PRIVATE, expected_read_pos, &timeout);
    if (futex_result != 0) {
      const int wait_errno = errno;
      result = ClassifyWriterWaitErrno(wait_errno);
      if (result == WriterWaitResult::kUnavailable) {
        errno = wait_errno;
        PERFETTO_DPLOG("tracing v2: futex wait on read_pos failed");
      }
    }
  }

  ring_header->num_writers_waiting.fetch_sub(1, std::memory_order_relaxed);
  return result;
#endif  // PERFETTO_TRACING_V2_HAS_FUTEX()
}

void SharedRingBuffer::PublishReadPos(uint32_t read_pos) {
  // The initial load is relaxed: it is only the first expected value for the
  // compare-and-swap below.
  PublishReadPosImpl(header()->rw_positions.load(std::memory_order_relaxed),
                     read_pos);
}

void SharedRingBuffer::PublishReadPosImpl(uint64_t expected_rw_positions,
                                          uint32_t read_pos) {
  RingBufferHeader* ring_header = header();

  // Only the reader changes read_pos, so this can lose only to a writer moving
  // write_pos. Release publishes the chunk reclaims before their capacity.
  // Failure only supplies the newer write_pos to preserve on the next attempt.
  while (!ring_header->rw_positions.compare_exchange_weak(
      expected_rw_positions, ReplaceReadPos(expected_rw_positions, read_pos),
      std::memory_order_release, std::memory_order_relaxed)) {
  }

#if PERFETTO_TRACING_V2_HAS_FUTEX()
  // Pairs with the fence in WaitForReadPosChange(); see the missed-wake
  // schedule there.
  std::atomic_thread_fence(std::memory_order_seq_cst);

  if (ring_header->num_writers_waiting.load(std::memory_order_relaxed) == 0)
    return;

  // Wake everyone: one drain pass can free many chunks, so waking a single
  // waiter would leave capacity unused. This runs once per pass, not once per
  // reclaimed chunk.
  //
  // Waits are bounded, so a failed wake delays writers but cannot strand them.
  if (FutexSyscall(ReadPosFutexWord(&ring_header->rw_positions),
                   FUTEX_WAKE_PRIVATE, static_cast<uint32_t>(INT32_MAX),
                   nullptr) < 0) {
    PERFETTO_DPLOG("tracing v2: futex wake on read_pos failed");
  }
#endif  // PERFETTO_TRACING_V2_HAS_FUTEX()
}

// --- Testing. ---

// static
std::optional<size_t> SharedRingBuffer::ComputeAllocationSizeForTesting(
    uint32_t num_chunks,
    uint32_t chunk_size,
    size_t page_size) {
  return ComputeAllocationSize(num_chunks, chunk_size, page_size);
}

// static
SharedRingBuffer::WriterWaitResult
SharedRingBuffer::ClassifyWriterWaitErrnoForTesting(int wait_errno) {
  return ClassifyWriterWaitErrno(wait_errno);
}

uint32_t SharedRingBuffer::num_writers_waiting_for_testing() const {
  return header()->num_writers_waiting.load(std::memory_order_relaxed);
}

uint32_t SharedRingBuffer::read_pos_for_testing() const {
  return ReadPosOf(header()->rw_positions.load(std::memory_order_relaxed));
}

SharedRingBuffer::Reservation SharedRingBuffer::TryReserveWritePosForTesting(
    uint64_t initial_rw_positions) {
  return TryReserveWritePosImpl(initial_rw_positions);
}

void SharedRingBuffer::PublishReadPosForTesting(uint64_t initial_rw_positions,
                                                uint32_t read_pos) {
  PublishReadPosImpl(initial_rw_positions, read_pos);
}

void SharedRingBuffer::SetStateWordForTesting(uint32_t chunk_index,
                                              uint32_t state_word) {
  chunk_state_word_at(chunk_index)
      ->store(state_word, std::memory_order_release);
}

void SharedRingBuffer::SetWritePosForTesting(uint32_t write_pos) {
  RingBufferHeader* ring_header = header();
  const uint64_t rw_positions =
      ring_header->rw_positions.load(std::memory_order_relaxed);
  ring_header->rw_positions.store(ReplaceWritePos(rw_positions, write_pos),
                                  std::memory_order_relaxed);
}

void SharedRingBuffer::SetPositionsForTesting(uint32_t position) {
  for (uint32_t chunk_index = 0; chunk_index < num_chunks_; ++chunk_index) {
    // The first position at or after |position| that maps to this chunk.
    const uint32_t first_position =
        position + ((chunk_index - position) & (num_chunks_ - 1));
    chunk_state_word_at(chunk_index)
        ->store(MakeFreeStateWord(
                    WrapCountForPosition(first_position, chunk_index_bits_)),
                std::memory_order_relaxed);
  }
  header()->rw_positions.store(PackRwPositions(position, position),
                               std::memory_order_release);
}

}  // namespace perfetto::tracing_v2
