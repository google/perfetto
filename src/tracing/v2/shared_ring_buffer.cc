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
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/paged_memory.h"
#include "perfetto/ext/base/utils.h"
#include "src/tracing/v2/tracing_v2_abi.h"

#if PERFETTO_TRACING_V2_HAS_FUTEX()
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif

namespace perfetto::tracing_v2 {
namespace {

bool IsLegalGeometry(uint32_t num_chunks, uint32_t chunk_size) {
  if (!base::IsPowerOfTwo(num_chunks) || num_chunks < kMinNumChunks ||
      num_chunks > kMaxNumChunks) {
    return false;
  }
  return base::IsPowerOfTwo(chunk_size) && chunk_size >= kMinChunkSize &&
         chunk_size <= kMaxChunkSize;
}

#if PERFETTO_TRACING_V2_HAS_FUTEX()
// A futex error the protocol has no answer for repeats on every stalled write,
// so it is logged once per process. One line naming the errno is what a person
// debugging a producer that will not stall needs; a line per write is noise
// that would push the interesting one off the screen.
std::atomic<bool> g_logged_wait_failure{false};
std::atomic<bool> g_logged_wake_failure{false};

bool ClaimOnceLog(std::atomic<bool>* logged) {
  return !logged->exchange(true, std::memory_order_relaxed);
}

// The cast is what SYS_futex wants: a plain 32-bit word in the mapping. That
// the atomic is exactly that word is asserted once, with the rest of the
// control-block layout, in the header.
//
// TODO(sashwinbalaji): the *_PRIVATE operations are correct only while the
// mapping never leaves the producer, which is the case in Step 1. Moving the
// consumer into traced means dropping the private flag; a private wait and a
// shared wake on the same address do not match.
int FutexSyscall(std::atomic<uint32_t>* word,
                 int op,
                 uint32_t value,
                 const struct timespec* timeout) {
  return static_cast<int>(syscall(SYS_futex, reinterpret_cast<uint32_t*>(word),
                                  op, value, timeout, nullptr, 0));
}
#endif  // PERFETTO_TRACING_V2_HAS_FUTEX()

}  // namespace

// static
std::unique_ptr<SharedRingBuffer> SharedRingBuffer::Create(
    uint32_t num_chunks,
    uint32_t chunk_size) {
  if (!IsLegalGeometry(num_chunks, chunk_size))
    return nullptr;

  // 64-bit throughout: num_chunks and chunk_size are individually bounded, but
  // their product is not representable in a uint32_t at the top of the range.
  const uint64_t total_size =
      uint64_t{kRingControlSize} + uint64_t{num_chunks} * uint64_t{chunk_size};
  // The geometry bounds alone allow a 32 TiB mapping, which is neither useful
  // nor representable in a size_t on the 32-bit hosts we still support. Cap it
  // so the cast below is exact everywhere.
  constexpr uint64_t kMaxRingBytes = uint64_t{1} << 31;
  if (total_size > kMaxRingBytes)
    return nullptr;

  // mmap(MAP_ANONYMOUS) memory is page-aligned and zero-filled, and
  // FreeForWrap(0) is the all-zero word, so the ring starts out with every
  // chunk already tagged for its first traversal and both cursors at zero.
  // That is why there is no O(num_chunks) stamping pass here.
  auto mapping = base::PagedMemory::Allocate(static_cast<size_t>(total_size),
                                             base::PagedMemory::kMayFail);
  if (!mapping.IsValid())
    return nullptr;

  return std::unique_ptr<SharedRingBuffer>(
      new SharedRingBuffer(std::move(mapping), num_chunks, chunk_size));
}

SharedRingBuffer::SharedRingBuffer(base::PagedMemory mapping,
                                   uint32_t num_chunks,
                                   uint32_t chunk_size)
    : mapping_(std::move(mapping)),
      mapping_start_(static_cast<uint8_t*>(mapping_.Get())),
      chunks_(mapping_start_ + kRingControlSize),
      num_chunks_(num_chunks),
      chunk_size_(chunk_size),
      chunk_bits_(Log2ForPowerOfTwo(num_chunks)),
      fragment_size_width_(FragmentSizeWidth(chunk_size)) {}

SharedRingBuffer::~SharedRingBuffer() = default;

SharedRingBuffer::Reservation SharedRingBuffer::ReservePosition() {
  RingControl* ctl = control();
  Reservation reservation{};
  for (;;) {
    // Acquire. A stale sample can only under-report capacity, which is safe.
    // What acquire buys is the other direction: if this does observe capacity
    // the reader has just exposed, it also makes the reader's preceding chunk
    // transition visible, so the claim below is not attempted against a state
    // word older than the reclaim that freed it.
    const uint32_t read_pos =
        ctl->reader.read_pos.load(std::memory_order_acquire);
    uint32_t write_pos = ctl->writer.write_pos.load(std::memory_order_relaxed);

    if (PositionDistance(write_pos, read_pos) >= num_chunks_) {
      reservation.outcome = ReserveOutcome::kFull;
      reservation.read_pos_sample = read_pos;
      return reservation;
    }

    // Relaxed: this allocates a number, nothing more. Ownership is arbitrated
    // on the chunk word.
    if (ctl->writer.write_pos.compare_exchange_weak(
            write_pos, write_pos + 1, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
      reservation.outcome = ReserveOutcome::kReserved;
      reservation.position = write_pos;
      reservation.read_pos_sample = read_pos;
      return reservation;
    }
    // A lost cursor CAS is pure contention: no position was obtained, so no
    // hole was created and no claim budget may be spent for it. Resample and
    // try again.
  }
}

bool SharedRingBuffer::TryClaim(uint32_t position, uint32_t acquired_word) {
  PERFETTO_DCHECK(StateOf(acquired_word) == ChunkState::kAcquired);
  PERFETTO_DCHECK(NumFragmentsOf(acquired_word) == 0);

  // The one word this reservation is entitled to swap against, derived from the
  // position and from nothing else.
  uint32_t expected =
      MakeFreeForWrapWord(WrapCountOfPosition(position, chunk_bits_));

  // Acquire on success consumes the reader's release reclaim, so the reader's
  // last reads of the previous traversal's payload happen before this writer's
  // first store into the chunk. On failure the caller discards the word that
  // compare_exchange leaves in |expected| and burns the position, so nothing is
  // interpreted and no ordering is needed.
  return state_at(ChunkIndexOfPosition(position, num_chunks_))
      ->compare_exchange_strong(expected, acquired_word,
                                std::memory_order_acquire,
                                std::memory_order_relaxed);
}

bool SharedRingBuffer::TryPublish(uint32_t chunk_index,
                                  uint32_t* observed,
                                  uint32_t complete_word) {
  PERFETTO_DCHECK(StateOf(*observed) == ChunkState::kAcquired);
  PERFETTO_DCHECK(StateOf(complete_word) == ChunkState::kComplete);

  // Release publishes the new fragments, their size entries and, on the first
  // publication, the target BufferID. On failure the reader has marked the
  // chunk, and acquire orders the reader's completed copy ahead of this
  // writer's relocation work.
  return state_at(chunk_index)
      ->compare_exchange_strong(*observed, complete_word,
                                std::memory_order_release,
                                std::memory_order_acquire);
}

bool SharedRingBuffer::TryReuse(uint32_t chunk_index, uint32_t observed) {
  PERFETTO_DCHECK(StateOf(observed) == ChunkState::kComplete);

  uint32_t expected = observed;
  // Relaxed on both. The successful compare-and-swap is a read-modify-write, so
  // it extends the release sequence of the publication that produced the
  // Complete word: a reader that acquire-loads the resulting Acquired word
  // still sees the already-published prefix. The reusing writer does not need
  // to acquire its own earlier stores. On failure the reader reclaimed the
  // chunk and the writer only drops its handle, so nothing is interpreted.
  return state_at(chunk_index)
      ->compare_exchange_strong(
          expected, WithState(observed, ChunkState::kAcquired),
          std::memory_order_relaxed, std::memory_order_relaxed);
}

bool SharedRingBuffer::TryAcknowledge(uint32_t chunk_index, uint32_t observed) {
  PERFETTO_DCHECK(StateOf(observed) == ChunkState::kRewriteRequested);

  uint32_t expected = observed;
  // Release means "every access I made under the old ownership is behind me".
  // Only this writer may leave RewriteRequested, so a failure is a protocol
  // error rather than a race, and the caller does not interpret the word.
  return state_at(chunk_index)
      ->compare_exchange_strong(expected, kAcknowledgedWord,
                                std::memory_order_release,
                                std::memory_order_relaxed);
}

uint32_t SharedRingBuffer::LoadStateAcquire(uint32_t chunk_index) const {
  // Pairs with every writer release transition, so each published fragment and
  // its size entry is visible before the reader walks and copies them.
  return state_at(chunk_index)->load(std::memory_order_acquire);
}

uint32_t SharedRingBuffer::LoadWritePosRelaxed() const {
  // A stale value can only make one drain pass report less data than exists.
  // Once a position is observed, ownership and payload visibility are decided
  // by the chunk word.
  return control()->writer.write_pos.load(std::memory_order_relaxed);
}

bool SharedRingBuffer::TryMarkForRewrite(uint32_t chunk_index,
                                         uint32_t* observed) {
  PERFETTO_DCHECK(StateOf(*observed) == ChunkState::kAcquired);

  // Release orders the reader's completed copy ahead of every later handoff of
  // this chunk. On failure the writer published more, and acquire makes those
  // extra fragments visible before the reader copies again.
  return state_at(chunk_index)
      ->compare_exchange_strong(
          *observed, WithState(*observed, ChunkState::kRewriteRequested),
          std::memory_order_release, std::memory_order_acquire);
}

bool SharedRingBuffer::TryAdvanceUnclaimed(uint32_t position,
                                           uint32_t* observed) {
  PERFETTO_DCHECK(StateOf(*observed) == ChunkState::kFreeForWrap);

  // Release makes this a direct handoff to the next claimant and orders any
  // earlier reader access to this chunk before its reuse. On failure a writer
  // claimed or published, and the reader goes on to interpret the returned
  // word, so it has to acquire it.
  return state_at(ChunkIndexOfPosition(position, num_chunks_))
      ->compare_exchange_strong(*observed, NextFreeWordFor(position),
                                std::memory_order_release,
                                std::memory_order_acquire);
}

bool SharedRingBuffer::TryReclaimComplete(uint32_t position,
                                          uint32_t* observed) {
  PERFETTO_DCHECK(StateOf(*observed) == ChunkState::kComplete);

  // Release orders the reader's copy ahead of the next writer's stores. On
  // failure the writer reused or published, and acquire makes the newly
  // published bytes visible before the reader redispatches. This
  // compare-and-swap is arbitration; it is not what validated the copy.
  return state_at(ChunkIndexOfPosition(position, num_chunks_))
      ->compare_exchange_strong(*observed, NextFreeWordFor(position),
                                std::memory_order_release,
                                std::memory_order_acquire);
}

bool SharedRingBuffer::TryReclaimAcknowledged(uint32_t position) {
  uint32_t expected = kAcknowledgedWord;
  // Acquire consumes the old writer's final release; release hands the chunk to
  // the next writer without leaning on release-sequence subtleties. Only the
  // reader may leave Acknowledged, so failure is a protocol error.
  return state_at(ChunkIndexOfPosition(position, num_chunks_))
      ->compare_exchange_strong(expected, NextFreeWordFor(position),
                                std::memory_order_acq_rel,
                                std::memory_order_relaxed);
}

SharedRingBuffer::WaitOutcome SharedRingBuffer::WaitForReadPosChange(
    uint32_t expected_read_pos,
    uint32_t timeout_ms) {
  PERFETTO_DCHECK(timeout_ms > 0);
#if !PERFETTO_TRACING_V2_HAS_FUTEX()
  base::ignore_result(expected_read_pos);
  base::ignore_result(timeout_ms);
  return WaitOutcome::kWaitUnavailable;
#else
  RingControl* ctl = control();

  // The waiter hint and read_pos are two independent atomics, and the two
  // seq_cst fences are what close a store-buffering cycle between them:
  //
  //   writer:  hint++            reader:  read_pos = new
  //            <fence>                    <fence>
  //            read read_pos              read hint
  //
  // Without the fences both reads may be satisfied from before the other side's
  // store. The writer then sees the old cursor and sleeps, the reader sees no
  // waiters and skips the wake, and the writer stays parked with capacity
  // available. This is the only seq_cst in the design: it closes this specific
  // cycle between two variables and says nothing about the chunk state machine,
  // which never needs it.
  ctl->writer.num_writers_waiting.fetch_add(1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_seq_cst);

  WaitOutcome outcome = WaitOutcome::kMayHaveProgressed;
  if (ctl->reader.read_pos.load(std::memory_order_relaxed) ==
      expected_read_pos) {
    struct timespec timeout = {};
    timeout.tv_sec = static_cast<time_t>(timeout_ms / 1000);
    timeout.tv_nsec = static_cast<long>((timeout_ms % 1000) * 1000000);

    // The kernel compares *read_pos against |expected_read_pos| atomically, so
    // a wake that lands between the load above and this call comes back as
    // EAGAIN rather than being lost. The whole errno policy is in
    // WaitOutcomeForWaitErrno(); note in particular that EINTR is deliberately
    // not retried here, because returning to the caller's capacity predicate is
    // the right answer to a signal.
    const int res = FutexSyscall(&ctl->reader.read_pos, FUTEX_WAIT_PRIVATE,
                                 expected_read_pos, &timeout);
    if (res != 0) {
      const int wait_errno = errno;
      outcome = WaitOutcomeForWaitErrno(wait_errno);
      if (outcome == WaitOutcome::kWaitUnavailable &&
          ClaimOnceLog(&g_logged_wait_failure)) {
        errno = wait_errno;
        PERFETTO_PLOG("tracing v2: futex wait on read_pos failed");
      }
    }
  }

  ctl->writer.num_writers_waiting.fetch_sub(1, std::memory_order_relaxed);
  return outcome;
#endif  // PERFETTO_TRACING_V2_HAS_FUTEX()
}

// static
SharedRingBuffer::WaitOutcome SharedRingBuffer::WaitOutcomeForWaitErrno(
    int wait_errno) {
  switch (wait_errno) {
    case ETIMEDOUT:
      return WaitOutcome::kTimedOut;
    case EAGAIN:
      // The kernel found read_pos already different from the value the caller
      // decided against, so the wait was never entered. That is progress, not
      // an error.
      return WaitOutcome::kMayHaveProgressed;
    case EINTR:
      // A signal. Going back to the caller's capacity predicate is deliberate:
      // it is the only thing that can tell the writer whether waiting again is
      // still the right thing to do.
      return WaitOutcome::kMayHaveProgressed;
    default:
      // EINVAL, EFAULT, ENOSYS and anything else mean this address cannot be
      // waited on at all. Reporting progress would send the caller straight
      // back into the same failing syscall.
      return WaitOutcome::kWaitUnavailable;
  }
}

void SharedRingBuffer::PublishReadPos(uint32_t read_pos) {
  RingControl* ctl = control();

  // Release: capacity is advertised only after the physical chunk transitions
  // that created it. A writer that acquires this value cannot then claim
  // against an older incarnation of that chunk.
  ctl->reader.read_pos.store(read_pos, std::memory_order_release);

#if PERFETTO_TRACING_V2_HAS_FUTEX()
  // See the schedule in WaitForReadPosChange() for why this fence is not
  // optional.
  std::atomic_thread_fence(std::memory_order_seq_cst);

  // The hint elides the syscall in the common uncontended case. It is never the
  // predicate for anything: a writer that misses the wake still re-evaluates
  // capacity when its wait returns.
  if (ctl->writer.num_writers_waiting.load(std::memory_order_relaxed) == 0)
    return;

  // Wake everyone: one drain pass can free many chunks, so waking a single
  // waiter would leave capacity unused. This runs once per pass, not once per
  // reclaimed chunk.
  //
  // A failed wake is not fatal - every waiter has a bounded slice and
  // re-evaluates capacity when it expires - but it does mean writers stall for
  // no reason, so it is worth one line.
  if (FutexSyscall(&ctl->reader.read_pos, FUTEX_WAKE_PRIVATE,
                   static_cast<uint32_t>(INT32_MAX), nullptr) < 0 &&
      ClaimOnceLog(&g_logged_wake_failure)) {
    PERFETTO_PLOG("tracing v2: futex wake on read_pos failed");
  }
#endif  // PERFETTO_TRACING_V2_HAS_FUTEX()
}

uint32_t SharedRingBuffer::num_writers_waiting_for_testing() const {
  return control()->writer.num_writers_waiting.load(std::memory_order_relaxed);
}

uint32_t SharedRingBuffer::read_pos_for_testing() const {
  return control()->reader.read_pos.load(std::memory_order_relaxed);
}

void SharedRingBuffer::SetStateWordForTesting(uint32_t chunk_index,
                                              uint32_t state_word) {
  state_at(chunk_index)->store(state_word, std::memory_order_release);
}

void SharedRingBuffer::SetWritePosForTesting(uint32_t write_pos) {
  control()->writer.write_pos.store(write_pos, std::memory_order_relaxed);
}

void SharedRingBuffer::SetCursorsForTesting(uint32_t position) {
  for (uint32_t chunk_index = 0; chunk_index < num_chunks_; ++chunk_index) {
    // The first position at or after |position| that maps to this chunk.
    const uint32_t first =
        position + ((chunk_index - position) & (num_chunks_ - 1));
    state_at(chunk_index)
        ->store(MakeFreeForWrapWord(WrapCountOfPosition(first, chunk_bits_)),
                std::memory_order_relaxed);
  }
  control()->writer.write_pos.store(position, std::memory_order_relaxed);
  control()->reader.read_pos.store(position, std::memory_order_release);
}

}  // namespace perfetto::tracing_v2
