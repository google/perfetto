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
// Linux/Android futex compares the aligned low 32-bit read_pos word while
// user space updates the containing aligned atomic64.
//
// The supported CPU/ABI set provides atomic observation of that 32-bit half
// during the 64-bit CAS.
uint32_t* ReadPosFutexWord(std::atomic<uint64_t>* rw_positions) {
  static_assert(PERFETTO_IS_LITTLE_ENDIAN(),
                "The low-word futex requires read_pos to be the first four "
                "bytes of rw_positions");
  return reinterpret_cast<uint32_t*>(rw_positions);
}

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
  // The reader marks chunks Free before publishing read_pos.
  //
  // Memory order acquire ensures that a writer which sees the new
  // read_pos also sees those Free state words.
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
    // The CAS can lose to:
    // - another writer reserving first.
    // - the reader publishing read_pos.
    //
    // Failure reloads both halves. The loop rechecks capacity, and no position
    // was taken.
    //
    // With memory order acquire, a snapshot that shows a newer read_pos
    // also shows the chunks the reader freed before publishing it.
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
  // The claim can fail because:
  // - the reader resolved this position as unclaimed.
  // - an older reservation still owns the chunk.
  //
  // Either way, this reservation is a hole and the caller never retries it.
  //
  // Memory order acquire pairs with the reader's memory order release
  // transition to Free. The writer cannot overwrite
  // the chunk until the reader is done with its old contents.
  //
  // On failure, memory order relaxed is enough because the returned word
  // is ignored.
  uint32_t expected = MakeFreeStateWordForPosition(position, num_chunks_);
  std::atomic<uint32_t>* state_word = chunk_state_word_for_position(position);
  return state_word->compare_exchange_strong(expected, being_written_word,
                                             std::memory_order_acquire,
                                             std::memory_order_relaxed);
}

bool SharedRingBuffer::TryReleaseChunkAsComplete(uint32_t chunk_idx,
                                                 uint32_t complete_word,
                                                 uint32_t* expected) {
  PERFETTO_DCHECK(ChunkStateOf(*expected) == ChunkState::kBeingWritten);
  PERFETTO_DCHECK(ChunkStateOf(complete_word) == ChunkState::kComplete);

  // BeingWritten(N) -> Complete(M).
  // The reader can request a rewrite first. Failure then returns
  // RewriteRequested(N), and the caller relocates the suffix.
  //
  // Memory order release publishes the M fragments and their sizes, plus
  // BufferID on the first publication.
  //
  // On failure, memory order acquire ensures that the reader has finished
  // copying the prefix before the writer sees its rewrite request.
  std::atomic<uint32_t>* state_word = chunk_state_word_at(chunk_idx);
  return state_word->compare_exchange_strong(*expected, complete_word,
                                             std::memory_order_release,
                                             std::memory_order_acquire);
}

bool SharedRingBuffer::TryReacquireChunkForWriting(uint32_t chunk_idx,
                                                   uint32_t observed) {
  PERFETTO_DCHECK(ChunkStateOf(observed) == ChunkState::kComplete);

  // Complete(N) -> BeingWritten(N).
  // The reader can reclaim the chunk first. The writer then drops its cached
  // handle and does not touch the chunk again.
  //
  // Success publishes no bytes, so it uses memory order relaxed.
  //
  // The read-modify-write still extends the release sequence of Complete(N).
  // The reader's memory order acquire load of BeingWritten(N) therefore
  // sees the prefix.
  //
  // Failure also uses memory order relaxed because the returned word is
  // ignored.
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
  // Only this writer may acknowledge, so failure is a protocol error.
  //
  // Memory order release orders the suffix copy before the reader
  // reclaims the chunk and a later writer overwrites it.
  //
  // On failure, memory order relaxed is enough because the unexpected
  // word is not inspected.
  uint32_t expected = observed;
  std::atomic<uint32_t>* state_word = chunk_state_word_at(chunk_idx);
  return state_word->compare_exchange_strong(
      expected, kRewriteAcknowledgedStateWord, std::memory_order_release,
      std::memory_order_relaxed);
}

// --- Reader-side chunk transitions. ---

uint32_t SharedRingBuffer::LoadChunkStateWord(uint32_t chunk_idx) const {
  // Memory order acquire pairs with the writer's memory order release
  // publication of Complete. This makes the
  // fragments, their size varints and BufferID visible to the reader.
  //
  // A BeingWritten word created by reuse extends that release sequence and
  // gives the same guarantee.
  return chunk_state_word_at(chunk_idx)->load(std::memory_order_acquire);
}

uint32_t SharedRingBuffer::LoadWritePos() const {
  // Memory order relaxed is enough because write_pos only bounds the
  // current drain pass.
  //
  // LoadChunkStateWord() uses memory order acquire to order the payload
  // reads.
  return WritePosOf(header()->rw_positions.load(std::memory_order_relaxed));
}

bool SharedRingBuffer::TryRequestRewrite(uint32_t chunk_idx,
                                         uint32_t* expected) {
  PERFETTO_DCHECK(ChunkStateOf(*expected) == ChunkState::kBeingWritten);

  // BeingWritten(N) -> RewriteRequested(N).
  // The writer can publish first. The reader then discards its copy and retries
  // this position on a later pass.
  //
  // Memory order release orders the prefix copy before the writer
  // observes the request and relocates its suffix.
  //
  // On failure, memory order relaxed is enough because the reader retries
  // this position without using the returned word to read the payload.
  std::atomic<uint32_t>* state_word = chunk_state_word_at(chunk_idx);
  return state_word->compare_exchange_strong(
      *expected, ReplaceChunkState(*expected, ChunkState::kRewriteRequested),
      std::memory_order_release, std::memory_order_relaxed);
}

bool SharedRingBuffer::TryMoveFreeChunkToNextWrap(uint32_t position,
                                                  uint32_t* expected) {
  PERFETTO_DCHECK(ChunkStateOf(*expected) == ChunkState::kFree);

  // Free(wrap(position)) -> Free(next wrap).
  // A delayed writer can claim first. The reader then retries this position on
  // a later pass and finds the chunk BeingWritten or Complete.
  //
  // Memory order release publishes Free to the next writer.
  //
  // On failure, memory order relaxed is enough because the reader retries
  // the position.
  std::atomic<uint32_t>* state_word = chunk_state_word_for_position(position);
  return state_word->compare_exchange_strong(
      *expected, MakeFreeWordForNextWrap(position), std::memory_order_release,
      std::memory_order_relaxed);
}

bool SharedRingBuffer::TryReleaseCompleteChunkAsFree(uint32_t position,
                                                     uint32_t* expected) {
  PERFETTO_DCHECK(ChunkStateOf(*expected) == ChunkState::kComplete);

  // Complete(N) -> Free(next wrap).
  // The writer can reuse the chunk first. The reader then discards its copy and
  // retries this position on a later pass.
  //
  // Memory order release orders the copy before the next writer acquires
  // Free and overwrites the chunk.
  //
  // On failure, memory order relaxed is enough because the reader retries
  // the position without using the copied payload.
  std::atomic<uint32_t>* state_word = chunk_state_word_for_position(position);
  return state_word->compare_exchange_strong(
      *expected, MakeFreeWordForNextWrap(position), std::memory_order_release,
      std::memory_order_relaxed);
}

bool SharedRingBuffer::TryReleaseRewriteAcknowledgedChunkAsFree(
    uint32_t position,
    uint32_t* observed) {
  // RewriteAcknowledged -> Free(next wrap).
  // Only this reader changes an acknowledged chunk. The expected value is the
  // one canonical RewriteAcknowledged word.
  //
  // Failure is therefore a protocol error. The caller gets the unexpected
  // word in |*observed|.
  //
  // Memory order acq_rel serves both handoffs:
  // - acquire pairs with the writer's acknowledgement after its suffix copy.
  // - release hands the chunk to the next writer after that copy.
  //
  // On failure, memory order relaxed is enough because the unexpected
  // word is used only to report the protocol error.
  uint32_t expected = kRewriteAcknowledgedStateWord;
  std::atomic<uint32_t>* state_word = chunk_state_word_for_position(position);
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

  // To avoid a missed wake:
  // - the writer counts itself as waiting before checking read_pos.
  // - the reader publishes read_pos before checking the waiter count.
  //
  // Without the two fences, both checks could see the old value. The writer
  // would then sleep after the reader skipped the wake:
  //
  //   writer                              reader
  //   ------                              ------
  //   num_writers_waiting += 1            publish read_pos
  //   memory order seq_cst fence          memory order seq_cst fence
  //   load read_pos                       load num_writers_waiting
  //   FUTEX_WAIT if unchanged             FUTEX_WAKE if nonzero
  //
  // Both fences use memory order seq_cst. Whichever comes first decides
  // the outcome:
  // - writer first: the reader sees the waiter and wakes it.
  // - reader first: the writer sees the new read_pos and does not sleep.
  //
  // num_writers_waiting is only a wake hint, so its accesses use
  // memory order relaxed.
  ring_header->num_writers_waiting.fetch_add(1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_seq_cst);

  WriterWaitResult result = WriterWaitResult::kRetry;
  const uint32_t read_pos =
      ReadPosOf(ring_header->rw_positions.load(std::memory_order_relaxed));
  if (read_pos == read_pos_for_wait) {
    struct timespec timeout{};
    timeout.tv_sec = static_cast<time_t>(timeout_ms / 1000);
    timeout.tv_nsec = static_cast<long>((timeout_ms % 1000) * 1000000);

    // FUTEX_WAIT checks the value again before sleeping. If read_pos changed
    // after the load above, the syscall returns EAGAIN and no wake is lost.
    if (FutexSyscall(ReadPosFutexWord(&ring_header->rw_positions),
                     FUTEX_WAIT_PRIVATE, read_pos_for_wait, &timeout) != 0) {
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
  // This load only supplies the initial expected value for the CAS below, so
  // memory order relaxed is enough.
  PublishReadPosFromSnapshot(
      header()->rw_positions.load(std::memory_order_relaxed), read_pos);
}

void SharedRingBuffer::PublishReadPosFromSnapshot(uint64_t rw_positions,
                                                  uint32_t read_pos) {
  RingBufferHeader* ring_header = header();

  // (write_pos, old read_pos) -> (write_pos, read_pos).
  // A writer can advance write_pos first.
  //
  // Failure reloads both halves. The retry keeps the new write_pos while
  // replacing read_pos again.
  //
  // Memory order release publishes this pass's Free words to writers that
  // see the new read_pos.
  //
  // On failure, memory order relaxed is enough because the returned value
  // only supplies the next CAS attempt.
  while (!ring_header->rw_positions.compare_exchange_weak(
      rw_positions, ReplaceReadPos(rw_positions, read_pos),
      std::memory_order_release, std::memory_order_relaxed)) {
  }

#if PERFETTO_TRACING_V2_HAS_FUTEX()
  // See the missed-wake schedule in WaitForReadPosChange(). The
  // memory order seq_cst fence must precede the waiter-count load.
  std::atomic_thread_fence(std::memory_order_seq_cst);

  if (ring_header->num_writers_waiting.load(std::memory_order_relaxed) == 0)
    return;

  // Wake everyone because one drain pass can free many chunks. Waking one
  // writer could leave capacity unused.
  //
  // This runs once per pass rather than once per reclaimed chunk.
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
