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

#include "perfetto/base/build_config.h"
#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/utils.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"

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

uint32_t NumChunksForRingLayout(const uint8_t* start,
                                size_t size,
                                uint32_t chunk_size) {
  PERFETTO_CHECK(start);
  PERFETTO_CHECK(
      reinterpret_cast<uintptr_t>(start) % alignof(RingBufferHeader) == 0);
  PERFETTO_CHECK(chunk_size >= kMinChunkSize);
  PERFETTO_CHECK(chunk_size % kChunkAlignmentBytes == 0);
  // Check before subtracting. Adding the header and chunk sizes can overflow
  // size_t on 32-bit builds.
  PERFETTO_CHECK(size >= sizeof(RingBufferHeader));
  const size_t chunks_size = size - sizeof(RingBufferHeader);
  PERFETTO_CHECK(chunks_size >= chunk_size);
  PERFETTO_CHECK(chunks_size % chunk_size == 0);
  const size_t num_chunks = chunks_size / chunk_size;
  PERFETTO_CHECK(num_chunks <= kMaxChunksPerRing);
  PERFETTO_CHECK(base::IsPowerOfTwo(num_chunks));
  return static_cast<uint32_t>(num_chunks);
}

#if PERFETTO_TRACING_V2_HAS_FUTEX()
// A futex operation acts on one aligned 32-bit word. The word writers park on
// is the low half of rw_positions.
uint32_t* ReadPosFutexWord(std::atomic<uint64_t>* rw_positions) {
  static_assert(PERFETTO_IS_LITTLE_ENDIAN(),
                "The low-word futex requires read_pos to be the first four "
                "bytes of rw_positions");
  return reinterpret_cast<uint32_t*>(rw_positions);
}

// TODO(sashwinbalaji): Drop *_PRIVATE when the reader moves into traced.
int FutexSyscall(uint32_t* word,
                 int op,
                 uint32_t value,
                 const struct timespec* timeout) {
  return static_cast<int>(
      syscall(SYS_futex, word, op, value, timeout, nullptr, 0));
}

SharedRingBuffer::WriterWaitResult ClassifyWriterWaitErrno(int wait_errno) {
  switch (wait_errno) {
    case ETIMEDOUT:
    case EAGAIN:
    case EINTR:
      return SharedRingBuffer::WriterWaitResult::kRetry;
    default:
      return SharedRingBuffer::WriterWaitResult::kUnavailable;
  }
}
#endif  // PERFETTO_TRACING_V2_HAS_FUTEX()

}  // namespace

// --- Construction. ---

SharedRingBuffer::SharedRingBuffer(uint8_t* start,
                                   size_t size,
                                   uint32_t chunk_size)
    : start_(start),
      num_chunks_(NumChunksForRingLayout(start, size, chunk_size)),
      chunk_size_(chunk_size) {}

// --- Writer-side reservation. ---

SharedRingBuffer::Reservation SharedRingBuffer::TryReserveWritePos() {
  // The reader marks chunks Free before advancing read_pos. Acquire ensures a
  // writer that sees the new read_pos also sees those Free state words.
  return TryReserveWritePosFromSnapshot(
      header()->rw_positions.load(std::memory_order_acquire));
}

SharedRingBuffer::Reservation SharedRingBuffer::TryReserveWritePosFromSnapshot(
    uint64_t rw_positions) {
  RingBufferHeader* ring_header = header();
  Reservation reservation{};
  for (;;) {
    const uint32_t write_pos = WritePosOf(rw_positions);
    const uint32_t read_pos = ReadPosOf(rw_positions);
    reservation.read_pos_for_wait = read_pos;

    if (NumOutstandingPositions(write_pos, read_pos) >= num_chunks_) {
      reservation.result = ReserveResult::kFull;
      return reservation;
    }

    // (write_pos, read_pos) -> (write_pos + 1, read_pos).
    // A writer reservation or reader update can win the race. On failure the
    // CAS reloads both positions; no reservation was made.
    if (ring_header->rw_positions.compare_exchange_weak(
            rw_positions, PackRwPositions(write_pos + 1, read_pos),
            std::memory_order_acquire, std::memory_order_acquire)) {
      reservation.result = ReserveResult::kReserved;
      reservation.position = write_pos;
      return reservation;
    }
  }
}

// --- Writer-side chunk transitions. ---

bool SharedRingBuffer::TryAcquireChunkForWriting(uint32_t position,
                                                 uint32_t being_written_word) {
  PERFETTO_DCHECK(ChunkStateOf(being_written_word) ==
                  ChunkState::kBeingWritten);
  PERFETTO_DCHECK(NumFragmentsOf(being_written_word) == 0);

  // Free(wrap(position)) -> BeingWritten(0).
  // - If the reader skipped this position first, it becomes a hole; never
  //   retry the claim against the next Free word.
  // - Acquire pairs with reclaim before this writer overwrites the payload.
  uint32_t expected =
      MakeFreeStateWord(WrapCountForPosition(position, num_chunks_));
  std::atomic<uint32_t>* state_word =
      chunk_state_word_at(ChunkIndexOfPosition(position, num_chunks_));
  return state_word->compare_exchange_strong(expected, being_written_word,
                                             std::memory_order_acquire,
                                             std::memory_order_relaxed);
}

bool SharedRingBuffer::TryReleaseChunkAsComplete(uint32_t chunk_idx,
                                                 uint32_t* observed,
                                                 uint32_t complete_word) {
  PERFETTO_DCHECK(ChunkStateOf(*observed) == ChunkState::kBeingWritten);
  PERFETTO_DCHECK(ChunkStateOf(complete_word) == ChunkState::kComplete);

  // BeingWritten(N) -> Complete(M).
  // - Success publishes the new payload and size varints with release.
  // - The reader can win with RewriteRequested(N). Failure acquire ensures its
  //   prefix copy finishes before the writer reads the suffix for relocation.
  std::atomic<uint32_t>* state_word = chunk_state_word_at(chunk_idx);
  return state_word->compare_exchange_strong(*observed, complete_word,
                                             std::memory_order_release,
                                             std::memory_order_acquire);
}

bool SharedRingBuffer::TryReacquireChunkForWriting(uint32_t chunk_idx,
                                                   uint32_t observed) {
  PERFETTO_DCHECK(ChunkStateOf(observed) == ChunkState::kComplete);

  // Complete(N) -> BeingWritten(N).
  // The reader can win by reclaiming the chunk. Relaxed is sufficient because
  // a successful RMW extends the release sequence that published the prefix.
  uint32_t expected = observed;
  std::atomic<uint32_t>* state_word = chunk_state_word_at(chunk_idx);
  return state_word->compare_exchange_strong(
      expected, ReplaceChunkState(observed, ChunkState::kBeingWritten),
      std::memory_order_relaxed, std::memory_order_relaxed);
}

bool SharedRingBuffer::TryAcknowledgeRewrite(uint32_t chunk_idx,
                                             uint32_t observed) {
  PERFETTO_DCHECK(ChunkStateOf(observed) == ChunkState::kRewriteRequested);

  // RewriteRequested -> RewriteAcknowledged.
  // Only its writer may leave RewriteRequested, so failure is a protocol
  // error. Release finishes copying the suffix before the reader reclaims it.
  uint32_t expected = observed;
  std::atomic<uint32_t>* state_word = chunk_state_word_at(chunk_idx);
  return state_word->compare_exchange_strong(
      expected, kRewriteAcknowledgedStateWord, std::memory_order_release,
      std::memory_order_relaxed);
}

// --- Reader-side chunk transitions. ---

uint32_t SharedRingBuffer::LoadChunkStateWord(uint32_t chunk_idx) const {
  // Acquire makes the published fragments and size varints visible.
  return chunk_state_word_at(chunk_idx)->load(std::memory_order_acquire);
}

uint32_t SharedRingBuffer::LoadWritePos() const {
  // This load only tells the reader that a position exists. Payload visibility
  // comes from the acquire load of the chunk state word, so relaxed is enough.
  return WritePosOf(header()->rw_positions.load(std::memory_order_relaxed));
}

bool SharedRingBuffer::TryRequestRewrite(uint32_t chunk_idx,
                                         uint32_t* observed) {
  PERFETTO_DCHECK(ChunkStateOf(*observed) == ChunkState::kBeingWritten);

  // BeingWritten(N) -> RewriteRequested(N).
  // - The writer can win with Complete(M); discard the prefix copy and retry.
  // - Release finishes the prefix copy before the writer relocates its suffix.
  std::atomic<uint32_t>* state_word = chunk_state_word_at(chunk_idx);
  return state_word->compare_exchange_strong(
      *observed, ReplaceChunkState(*observed, ChunkState::kRewriteRequested),
      std::memory_order_release, std::memory_order_relaxed);
}

bool SharedRingBuffer::TryMoveFreeChunkToNextWrap(uint32_t position,
                                                  uint32_t* observed) {
  PERFETTO_DCHECK(ChunkStateOf(*observed) == ChunkState::kFree);

  // Free(wrap(position)) -> Free(next wrap).
  // The delayed writer can win the claim; retry as BeingWritten. Release
  // orders earlier reader accesses before the next writer overwrites the chunk.
  std::atomic<uint32_t>* state_word =
      chunk_state_word_at(ChunkIndexOfPosition(position, num_chunks_));
  return state_word->compare_exchange_strong(
      *observed, MakeFreeWordForNextWrap(position), std::memory_order_release,
      std::memory_order_relaxed);
}

bool SharedRingBuffer::TryReleaseCompleteChunkAsFree(uint32_t position,
                                                     uint32_t* observed) {
  PERFETTO_DCHECK(ChunkStateOf(*observed) == ChunkState::kComplete);

  // Complete -> Free(next wrap).
  // The writer can win by reacquiring the chunk; discard the copy and retry.
  // Release finishes the copy before the next writer overwrites the chunk.
  std::atomic<uint32_t>* state_word =
      chunk_state_word_at(ChunkIndexOfPosition(position, num_chunks_));
  return state_word->compare_exchange_strong(
      *observed, MakeFreeWordForNextWrap(position), std::memory_order_release,
      std::memory_order_relaxed);
}

bool SharedRingBuffer::TryReleaseRewriteAcknowledgedChunkAsFree(
    uint32_t position,
    uint32_t* observed) {
  // RewriteAcknowledged -> Free(next wrap).
  // No other actor may change RewriteAcknowledged, so failure is a protocol
  // error. Acquire waits for the suffix copy; release finishes it before the
  // next writer overwrites the chunk.
  uint32_t expected = kRewriteAcknowledgedStateWord;
  std::atomic<uint32_t>* state_word =
      chunk_state_word_at(ChunkIndexOfPosition(position, num_chunks_));
  const bool reclaimed = state_word->compare_exchange_strong(
      expected, MakeFreeWordForNextWrap(position), std::memory_order_acq_rel,
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
    uint32_t read_pos_for_wait,
    uint32_t timeout_ms) {
  PERFETTO_DCHECK(timeout_ms > 0);
#if !PERFETTO_TRACING_V2_HAS_FUTEX()
  base::ignore_result(read_pos_for_wait);
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
      read_pos_for_wait) {
    struct timespec timeout{};
    timeout.tv_sec = static_cast<time_t>(timeout_ms / 1000);
    timeout.tv_nsec = static_cast<long>((timeout_ms % 1000) * 1000000);

    // FUTEX_WAIT checks the value again before sleeping. If read_pos changed
    // after the load above, the syscall returns EAGAIN and no wake is lost.
    const int futex_result =
        FutexSyscall(ReadPosFutexWord(&ring_header->rw_positions),
                     FUTEX_WAIT_PRIVATE, read_pos_for_wait, &timeout);
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
  PublishReadPosFromSnapshot(
      header()->rw_positions.load(std::memory_order_relaxed), read_pos);
}

void SharedRingBuffer::PublishReadPosFromSnapshot(uint64_t rw_positions,
                                                  uint32_t read_pos) {
  RingBufferHeader* ring_header = header();

  // (write_pos, old read_pos) -> (write_pos, read_pos).
  // Writers can race by advancing write_pos; failure reloads it. Release makes
  // the Free state words visible to writers that observe the new read_pos.
  while (!ring_header->rw_positions.compare_exchange_weak(
      rw_positions, ReplaceReadPos(rw_positions, read_pos),
      std::memory_order_release, std::memory_order_relaxed)) {
  }

#if PERFETTO_TRACING_V2_HAS_FUTEX()
  // Pairs with the fence in WaitForReadPosChange(); see the missed-wake
  // schedule there.
  std::atomic_thread_fence(std::memory_order_seq_cst);

  if (ring_header->num_writers_waiting.load(std::memory_order_relaxed) == 0)
    return;

  // Wake everyone: one drain pass can free many chunks, so waking a single
  // waiter would leave capacity unused. This runs once per pass rather than
  // once per reclaimed chunk.
  //
  // Waits are bounded, so a failed wake delays writers but cannot strand them.
  if (FutexSyscall(ReadPosFutexWord(&ring_header->rw_positions),
                   FUTEX_WAKE_PRIVATE, static_cast<uint32_t>(INT32_MAX),
                   nullptr) < 0) {
    PERFETTO_DPLOG("tracing v2: futex wake on read_pos failed");
  }
#endif  // PERFETTO_TRACING_V2_HAS_FUTEX()
}

}  // namespace perfetto::tracing_v2
