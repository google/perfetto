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
#include "perfetto/protozero/proto_utils.h"
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

static_assert(kMaxFragmentSizeVarIntBytes ==
                  protozero::proto_utils::kMessageLengthFieldSize,
              "Ring buffer fragment sizes must use the same varint byte limit "
              "as Protozero message lengths");

uint32_t NumChunksForRingLayout(const uint8_t* start,
                                size_t size,
                                uint32_t chunk_size) {
  PERFETTO_CHECK(start);
  PERFETTO_CHECK(
      reinterpret_cast<uintptr_t>(start) % alignof(RingBufferHeader) == 0);
  PERFETTO_CHECK(chunk_size >= kMinChunkSize);
  PERFETTO_CHECK(chunk_size % kChunkAlignmentBytes == 0);
  // Subtract the header after this check to avoid overflow on 32-bit builds.
  PERFETTO_CHECK(size >= sizeof(RingBufferHeader));
  const size_t chunks_size = size - sizeof(RingBufferHeader);
  PERFETTO_CHECK(chunks_size % chunk_size == 0);
  const size_t num_chunks = chunks_size / chunk_size;

  // We require two chunks as the minimum useful configuration.
  // One chunk would still work with the ABI:
  // - write_pos - read_pos is 0 when empty and 1 when full.
  // - The Free wrap count distinguishes successive uses of the chunk.
  PERFETTO_CHECK(num_chunks >= kMinChunksPerRing);
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
  // A writer that sees the new read_pos also sees those Free state words.
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
    //
    // On failure, no position is reserved. The CAS stores the current write_pos
    // and read_pos in the local rw_positions value. The loop rechecks capacity
    // before another attempt. A competing update can come from:
    // - Another writer that advances write_pos.
    // - The reader that publishes read_pos.
    //
    // compare_exchange_weak can also fail spuriously.
    //
    // Proposed memory ordering:
    // - Success: acquire to observe the Free words published with read_pos.
    // - Failure: acquire for the same reason. The refreshed read_pos feeds
    //   the next capacity check.
    if (ring_header->rw_positions.compare_exchange_weak(
            rw_positions, PackRwPositions(write_pos + 1, read_pos))) {
      reservation.result = ReserveResult::kReserved;
      reservation.chunk_pos = write_pos;
      return reservation;
    }
  }
}

// --- Writer-side chunk transitions. ---

bool SharedRingBuffer::TryAcquireChunkForWriting(uint32_t chunk_pos,
                                                 uint32_t being_written_word) {
  PERFETTO_DCHECK(ChunkStateOf(being_written_word) ==
                  ChunkState::kBeingWritten);
  PERFETTO_DCHECK(NumFragmentsOf(being_written_word) == 0);

  // Free(wrap_count(chunk_pos)) -> BeingWritten(0).
  //
  // On failure, the chunk does not match this reservation's Free word:
  // - The reader advanced the wrap count before this writer claimed the chunk.
  // - A writer from an earlier reservation still owns the physical chunk, or
  //   the reader has not reclaimed its RewriteAcknowledged state yet.
  //
  // The caller must reserve a new position. A retry against the returned word
  // can claim a consumed position or overwrite another writer's chunk.
  //
  // Proposed memory ordering:
  // - Success: acquire to observe the reader's release of Free.
  //   The reader must finish copying before this writer overwrites the chunk.
  // - Failure: relaxed because the returned word is ignored.
  uint32_t expected = MakeFreeStateWordForPosition(chunk_pos, num_chunks_);
  std::atomic<uint32_t>* state_word = chunk_state_word_for_position(chunk_pos);
  return state_word->compare_exchange_strong(expected, being_written_word);
}

bool SharedRingBuffer::TryReleaseChunkAsComplete(ChunkIndex chunk_idx,
                                                 uint32_t complete_word,
                                                 uint32_t* expected) {
  PERFETTO_DCHECK(ChunkStateOf(*expected) == ChunkState::kBeingWritten);
  PERFETTO_DCHECK(ChunkStateOf(complete_word) == ChunkState::kComplete);

  // BeingWritten(N) -> Complete(M).
  //
  // On failure, the reader requested a rewrite first. |*expected| receives
  // RewriteRequested(N), and the caller relocates any unpublished fragment.
  //
  // Proposed memory ordering:
  // - Success: release to publish M fragments and their sizes.
  //   The first publication also makes BufferID visible.
  // - Failure: acquire to observe the reader's RewriteRequested(N).
  //   The reader must finish copying N fragments before the writer relocates.
  std::atomic<uint32_t>* state_word = chunk_state_word_at(chunk_idx);
  return state_word->compare_exchange_strong(*expected, complete_word);
}

bool SharedRingBuffer::TryReacquireChunkForWriting(ChunkIndex chunk_idx,
                                                   uint32_t observed) {
  PERFETTO_DCHECK(ChunkStateOf(observed) == ChunkState::kComplete);

  // Complete(N) -> BeingWritten(N).
  //
  // On failure, the reader reclaimed the chunk first. The writer discards its
  // cached handle and acquires another chunk.
  //
  // Proposed memory ordering:
  // - Success: relaxed because reuse publishes no new bytes.
  //   The read-modify-write extends Complete(N)'s release sequence.
  //   The reader's acquire load still sees the N published fragments.
  // - Failure: relaxed because the returned word is ignored.
  uint32_t expected = observed;
  std::atomic<uint32_t>* state_word = chunk_state_word_at(chunk_idx);
  return state_word->compare_exchange_strong(
      expected, ReplaceChunkState(observed, ChunkState::kBeingWritten));
}

bool SharedRingBuffer::TryAcknowledgeRewrite(ChunkIndex chunk_idx,
                                             uint32_t observed) {
  PERFETTO_DCHECK(ChunkStateOf(observed) == ChunkState::kRewriteRequested);

  // RewriteRequested -> RewriteAcknowledged, with all other bits zero.
  //
  // On failure, report a protocol error. Only this writer can change
  // RewriteRequested, so the observed word must still match.
  //
  // Proposed memory ordering:
  // - Success: release to save the unpublished fragment before
  //   acknowledgement. The reader can then reclaim the chunk.
  // - Failure: relaxed because it only reports a protocol error.
  uint32_t expected = observed;
  std::atomic<uint32_t>* state_word = chunk_state_word_at(chunk_idx);
  return state_word->compare_exchange_strong(expected,
                                             kRewriteAcknowledgedStateWord);
}

// --- Reader-side chunk transitions. ---

uint32_t SharedRingBuffer::LoadChunkStateWordAcquire(
    ChunkIndex chunk_idx) const {
  // The acquire load pairs with the writer's release publication of fragments
  // and their sizes. The first publication also makes BufferID visible.
  //
  // If the writer reuses Complete(N) as BeingWritten(N), its CAS preserves the
  // fragment count and extends the release sequence. This load still makes
  // those N fragments, their sizes and BufferID visible when N is nonzero.
  return chunk_state_word_at(chunk_idx)->load(std::memory_order_acquire);
}

uint32_t SharedRingBuffer::LoadWritePosRelaxed() const {
  // This bounds the reader's pass. It does not show whether each reservation
  // has published data. LoadChunkStateWordAcquire() provides that information.
  return WritePosOf(header()->rw_positions.load(std::memory_order_relaxed));
}

bool SharedRingBuffer::TryRequestRewrite(ChunkIndex chunk_idx,
                                         uint32_t* expected) {
  PERFETTO_DCHECK(ChunkStateOf(*expected) == ChunkState::kBeingWritten);

  // BeingWritten(N) -> RewriteRequested(N).
  // |*expected| is the word the reader used to copy the N fragments.
  //
  // On failure, the writer published first. The reader discards its copy and
  // retries this position immediately in Drain() to include the new fragments.
  //
  // Proposed memory ordering:
  // - Success: release to finish that copy before the writer observes
  //   RewriteRequested and relocates its unpublished fragment.
  // - Failure: relaxed because the copy is discarded. The next attempt
  //   acquire-loads the state word again before reading any payload.
  std::atomic<uint32_t>* state_word = chunk_state_word_at(chunk_idx);
  return state_word->compare_exchange_strong(
      *expected, ReplaceChunkState(*expected, ChunkState::kRewriteRequested));
}

bool SharedRingBuffer::TryMoveFreeChunkToNextWrap(uint32_t chunk_pos,
                                                  uint32_t* expected) {
  PERFETTO_DCHECK(ChunkStateOf(*expected) == ChunkState::kFree);

  // Free(wrap_count(chunk_pos)) -> Free(next wrap).
  //
  // On failure, the delayed writer claimed the chunk first. The reader retries
  // this position immediately in Drain() to consume BeingWritten or Complete.
  //
  // Proposed memory ordering:
  // - Success: release to publish the next Free wrap to a claiming writer.
  // - Failure: relaxed because the next attempt acquire-loads the state
  //   word again before reading any payload.
  std::atomic<uint32_t>* state_word = chunk_state_word_for_position(chunk_pos);
  return state_word->compare_exchange_strong(
      *expected, MakeFreeWordForNextWrap(chunk_pos));
}

bool SharedRingBuffer::TryReleaseCompleteChunkAsFree(uint32_t chunk_pos,
                                                     uint32_t* expected) {
  PERFETTO_DCHECK(ChunkStateOf(*expected) == ChunkState::kComplete);

  // Complete(N) -> Free(next wrap).
  // |*expected| is the word the reader used to copy the N fragments.
  //
  // On failure, the writer reused the chunk first. It can also publish more
  // fragments before the reader retries, so the N-fragment copy can be stale.
  // Drain() reloads the state and copies the current published fragments.
  // If the state is BeingWritten, the reader then requests a rewrite.
  // Otherwise, it tries to release Complete again. The reader delivers the
  // copy only after a successful transition, so retries cannot duplicate data.
  //
  // Proposed memory ordering:
  // - Success: release to finish the copy before another writer acquires
  //   Free and overwrites the chunk.
  // - Failure: relaxed because the copy is discarded. The next attempt
  //   acquire-loads the state word again before reading any payload.
  std::atomic<uint32_t>* state_word = chunk_state_word_for_position(chunk_pos);
  return state_word->compare_exchange_strong(
      *expected, MakeFreeWordForNextWrap(chunk_pos));
}

bool SharedRingBuffer::TryReleaseRewriteAcknowledgedChunkAsFree(
    uint32_t chunk_pos,
    uint32_t* observed) {
  // RewriteAcknowledged -> Free(next wrap).
  // The expected word contains only the RewriteAcknowledged state bits.
  //
  // On failure, |*observed| receives the unexpected word. This is a protocol
  // error because only this reader can change an acknowledged chunk.
  //
  // Proposed memory ordering:
  // - Success: acq_rel for two handoffs:
  //   - Acquire observes the writer's acknowledgement after it saves its
  //     unpublished fragment.
  //   - Release passes the chunk to the next writer after that copy.
  // - Failure: relaxed because the returned word only diagnoses the
  //   protocol error.
  uint32_t expected = kRewriteAcknowledgedStateWord;
  std::atomic<uint32_t>* state_word = chunk_state_word_for_position(chunk_pos);
  const bool reclaimed = state_word->compare_exchange_strong(
      expected, MakeFreeWordForNextWrap(chunk_pos));
  if (PERFETTO_UNLIKELY(!reclaimed))
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
  // The fences order each update before the other side's check. Together
  // they prevent both checks from seeing an older value:
  //
  //   writer                         reader
  //   ------                         ------
  //   num_writers_waiting += 1       publish read_pos
  //   seq_cst fence                  seq_cst fence
  //   load read_pos                  load num_writers_waiting
  //   FUTEX_WAIT if unchanged        FUTEX_WAKE if nonzero
  //
  // - Writer's fence first: the reader sees the waiter and wakes it.
  // - Reader's fence first: the writer sees the new read_pos.
  //   It does not sleep.
  //
  // num_writers_waiting is only a wake hint, so its accesses use relaxed order.
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
  // This load only supplies the initial expected value for the CAS below.
  PublishReadPosFromSnapshot(
      header()->rw_positions.load(std::memory_order_relaxed), read_pos);
}

void SharedRingBuffer::PublishReadPosFromSnapshot(uint64_t rw_positions,
                                                  uint32_t read_pos) {
  RingBufferHeader* ring_header = header();

  // (write_pos, old read_pos) -> (write_pos, read_pos).
  //
  // On failure, the CAS stores the current positions in the local rw_positions.
  // A writer can advance write_pos first, or compare_exchange_weak can fail
  // spuriously. The loop preserves that write_pos and retries the read_pos
  // update.
  //
  // Proposed memory ordering:
  // - Success: release so writers that acquire the new read_pos also see
  //   this pass's Free words.
  // - Failure: relaxed because the returned word only supplies the next
  //   CAS attempt. No payload access depends on it.
  while (!ring_header->rw_positions.compare_exchange_weak(
      rw_positions, ReplaceReadPos(rw_positions, read_pos))) {
  }

#if PERFETTO_TRACING_V2_HAS_FUTEX()
  // See the missed-wake schedule in WaitForReadPosChange(). The fence
  // orders read_pos publication before the waiter-count load.
  std::atomic_thread_fence(std::memory_order_seq_cst);

  if (ring_header->num_writers_waiting.load(std::memory_order_relaxed) == 0)
    return;

  // Wake everyone because one drain pass can free many chunks. Waking one
  // writer could leave capacity unused.
  //
  // This runs once per pass rather than once per reclaimed chunk.
  //
  // Waits are bounded, so a failed wake delays writers but cannot strand them.
  if (PERFETTO_UNLIKELY(
          FutexSyscall(ReadPosFutexWord(&ring_header->rw_positions),
                       FUTEX_WAKE_PRIVATE, static_cast<uint32_t>(INT32_MAX),
                       nullptr) < 0)) {
    PERFETTO_DPLOG("tracing v2: futex wake on read_pos failed");
  }
#endif  // PERFETTO_TRACING_V2_HAS_FUTEX()
}

}  // namespace perfetto::tracing_v2
