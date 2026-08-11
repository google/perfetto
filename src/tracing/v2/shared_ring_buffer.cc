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

#include <string.h>

#include <array>
#include <limits>
#include <new>
#include <utility>

#include "perfetto/ext/base/utils.h"

// PERFETTO_TRACING_V2_HAS_FUTEX() comes from shared_ring_buffer.h.
#if PERFETTO_TRACING_V2_HAS_FUTEX()
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif

namespace perfetto {
namespace tracing_v2 {

namespace {

#if PERFETTO_TRACING_V2_HAS_FUTEX()

// FUTEX_WAIT/FUTEX_WAKE operate on a bare uint32_t. std::atomic<uint32_t> is
// layout-compatible with one as long as it is lock-free, i.e. has no side
// state the kernel would be unaware of. Both asserts below pin that down.
static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t),
              "std::atomic<uint32_t> must be futex-compatible");
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "std::atomic<uint32_t> must be futex-compatible");

// FUTEX_*_PRIVATE keys the kernel-side wait queue on the calling process'
// address space, which is both what we want while the ring is in-process and
// markedly cheaper. When the ring moves into a memfd shared with traced this
// has to lose the _PRIVATE suffix, or cross-process wakeups are silently
// dropped and stalled writers only come back on the timeout.
// TODO(sashwinbalaji): drop _PRIVATE together with the memfd handover.
// The three errno values a caller can do something about: all three mean "go
// re-check the real predicate". Anything else is a wrong address, a wrong
// timeout or a kernel without the syscall, none of which a retry can fix.
bool IsRetryableFutexErrno(int err) {
  return err == EAGAIN || err == EINTR || err == ETIMEDOUT;
}

void FutexWait(std::atomic<uint32_t>* addr,
               uint32_t expected,
               uint32_t timeout_ms) {
  struct timespec timeout = {};
  timeout.tv_sec = static_cast<time_t>(timeout_ms / 1000);
  timeout.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1000000L;
  // The kernel compares *addr against |expected| atomically and returns EAGAIN
  // if they already differ. That comparison is what makes the wakeup
  // impossible to miss: a reader that bumped the generation after our caller
  // sampled it fails this wait instead of leaving us asleep.
  errno = 0;
  const long res = syscall(SYS_futex, reinterpret_cast<uint32_t*>(addr),
                           FUTEX_WAIT_PRIVATE, expected, &timeout, nullptr, 0);
  if (PERFETTO_LIKELY(res == 0))
    return;
  const int err = errno;
  // Deliberately no PERFETTO_EINTR: re-entering the same syscall would skip
  // the caller's predicate re-check.
  if (PERFETTO_LIKELY(IsRetryableFutexErrno(err)))
    return;
  // Returning here would look like a spurious wakeup to the caller, which
  // would come straight back and turn a stalled writer into a hot syscall and
  // logging loop. It also cannot honour kStall any more, so stop instead.
  PERFETTO_FATAL("Tracing v2 FUTEX_WAIT failed, errno=%d", err);
}

void FutexWakeAll(std::atomic<uint32_t>* addr) {
  errno = 0;
  const long res = syscall(SYS_futex, reinterpret_cast<uint32_t*>(addr),
                           FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
  // A wake with nobody queued legitimately returns 0. Negative means a bad
  // address or a bad opcode: every stalled writer would then be left to its
  // timeout slices with no way to notice, so fail here instead.
  if (PERFETTO_UNLIKELY(res < 0))
    PERFETTO_FATAL("Tracing v2 FUTEX_WAKE failed, errno=%d", errno);
}

#endif  // PERFETTO_TRACING_V2_HAS_FUTEX()

size_t RingAllocationSize(uint32_t num_chunks) {
  PERFETTO_CHECK(num_chunks >= 2 && base::IsPowerOfTwo(num_chunks));
  PERFETTO_CHECK(num_chunks < (1u << 31));
  PERFETTO_CHECK(static_cast<size_t>(num_chunks) <=
                 std::numeric_limits<size_t>::max() / kChunkSize - 1);
  return (static_cast<size_t>(num_chunks) + 1) * kChunkSize;
}

}  // namespace

SharedRingBuffer::Chunk::Chunk(Chunk&& other) noexcept
    : begin_(other.begin_),
      state_word_(other.state_word_),
      payload_used_(other.payload_used_) {
  // If you add a member to Chunk, remember to move it here too.
  other.begin_ = nullptr;
  other.state_word_ = 0;
  other.payload_used_ = 0;
}

SharedRingBuffer::Chunk& SharedRingBuffer::Chunk::operator=(
    Chunk&& other) noexcept {
  if (this != &other) {
    this->~Chunk();
    new (this) Chunk(std::move(other));
  }
  return *this;
}

SharedRingBuffer::SharedRingBuffer(uint32_t num_chunks)
    : mem_(base::PagedMemory::Allocate(RingAllocationSize(num_chunks))),
      start_(static_cast<uint8_t*>(mem_.Get())),
      num_chunks_(num_chunks),
      chunk_index_mask_(num_chunks - 1) {
  // PagedMemory provides suitably aligned raw storage, not live C++ objects.
  // Placement construction starts the C++17 lifetime of the control header and
  // of each atomic state word before any thread accesses them.
  new (start_) RingBufferHeader();
  for (uint32_t i = 0; i < num_chunks_; ++i)
    new (state_word(chunk_at(i))) std::atomic<uint32_t>(kFreeStateWord);
}

SharedRingBuffer::~SharedRingBuffer() {
  // Unread chunks at teardown are legitimate: a session can stop while the
  // relay is still behind, and the producer is not obliged to drain first.
  ring_buffer_header()->~RingBufferHeader();
}

bool SharedRingBuffer::TryReserveWritePos(uint32_t* pos) {
  std::atomic<uint32_t>& shared_write_pos = ring_buffer_header()->write_pos;
  for (;;) {
    // Both cursors are sampled inside the loop, reader first. Carrying a write
    // candidate across the capacity check would let this thread be descheduled
    // while other writers and the reader move past it, after which
    // |candidate - reader_pos| wraps to a huge unsigned value and reports a
    // full ring that has room. Safe, but it drops or stalls a writer for no
    // reason.
    //
    // acquire pairs with the reader's release-store after it has finished with
    // every physical slot preceding this position.
    const uint32_t reader_pos =
        ring_buffer_header()->read_pos.load(std::memory_order_acquire);
    uint32_t candidate = shared_write_pos.load(std::memory_order_relaxed);
    if (candidate - reader_pos >= num_chunks_)
      return false;

    // This publishes a FIFO reservation, not bytes or physical ownership. The
    // state-word CAS below is the ownership linearization point.
    if (shared_write_pos.compare_exchange_weak(candidate, candidate + 1,
                                               std::memory_order_relaxed,
                                               std::memory_order_relaxed)) {
      *pos = candidate;
      return true;
    }
    // Deliberately discard the value the failed CAS wrote back into
    // |candidate|: the next iteration takes a fresh reader-then-writer
    // snapshot instead of reusing half of this one.
  }
}

SharedRingBuffer::Chunk SharedRingBuffer::TryClaimReservedChunk(
    const ChunkHeader& requested_header,
    uint32_t pos) {
  uint8_t* const chunk = chunk_at(pos);
  auto* const state = state_word(chunk);

  ChunkHeader writing_header = requested_header;
  PERFETTO_DCHECK(writing_header.writer_id != 0);
  // Version 0 defines no extended header. Keep the DCHECK for bad callers and
  // sanitize in release so this writer cannot publish a layout it cannot fill.
  PERFETTO_DCHECK(!writing_header.extended_header);
  writing_header.version = kChunkVersion;
  writing_header.extended_header = false;
  writing_header.payload_size = 0;  // Nothing committed in this slot yet.
  writing_header.flags = static_cast<uint8_t>(
      (requested_header.flags & kChunkFlagsMask) | kFlagAcquiredForWriting);
  const uint32_t writing_state = writing_header.ToStateWord();

  uint32_t expected = kFreeStateWord;
  // acquire consumes the reader's release when this physical slot was freed,
  // so no previous reader still accesses bytes that we are about to replace.
  if (!state->compare_exchange_strong(expected, writing_state,
                                      std::memory_order_acquire,
                                      std::memory_order_relaxed)) {
    if (expected == kInvalidatedChunkHeader) {
      // The reader passed a previously reserved-but-unclaimed incarnation of
      // this slot. Nobody wrote bytes into that invalidated slot. Failure to
      // free is benign: another delayed writer may already have freed it and a
      // later writer may own the next incarnation. In either case this writer
      // has no remaining state to transition.
      state->compare_exchange_strong(expected, kFreeStateWord,
                                     std::memory_order_release,
                                     std::memory_order_relaxed);
    }
    return Chunk();
  }

  StoreChunkTargetBuffer(chunk, writing_header.target_buffer);

  // The reader can pass a published reservation before its writer claims the
  // slot. Reject this claim if that happened. This prevents a delayed writer
  // from publishing stale bytes after a complete ring lap.
  const uint32_t reader_pos =
      ring_buffer_header()->read_pos.load(std::memory_order_acquire);
  if (IsPositionAtOrAfter(reader_pos, pos + 1)) {
    // We own either |writing_state| or |writing_state|NeedsRewrite. In both
    // cases the reader has already skipped it and will never inspect payload.
    state->store(kFreeStateWord, std::memory_order_release);
    return Chunk();
  }

  return Chunk(chunk, writing_state);
}

SharedRingBuffer::Chunk SharedRingBuffer::TryAcquireChunkForWriting(
    const ChunkHeader& header) {
  PERFETTO_DCHECK(header.writer_id != 0);
  for (;;) {
    uint32_t pos = 0;
    if (!TryReserveWritePos(&pos))
      return Chunk();
    Chunk chunk = TryClaimReservedChunk(header, pos);
    if (chunk.is_valid())
      return chunk;
    // The reservation remains a logical hole. The reader consumes it exactly
    // once, while this writer moves on to a later FIFO position.
  }
}

bool SharedRingBuffer::TryReacquireChunkForWriting(Chunk* chunk) {
  PERFETTO_DCHECK(chunk && chunk->is_valid() && !chunk->is_being_written());
  // The published length is the only place the appending writer may start, and
  // the reader may be copying everything below it right now.
  PERFETTO_DCHECK(chunk->payload_used_ == chunk->committed_payload_size());
  auto* const state = state_word(chunk->begin_);
  const uint32_t desired = chunk->state_word_ | kFlagAcquiredForWriting;
  uint32_t expected = chunk->state_word_;
  // Fails if the reader freed the slot, or if a later incarnation of it now
  // belongs to somebody else. It does not fail against a reader that is merely
  // copying the committed prefix: that reader owns bytes we will not touch,
  // and our release below is what makes its CAS fail instead.
  if (state->compare_exchange_strong(expected, desired,
                                     std::memory_order_acquire,
                                     std::memory_order_relaxed)) {
    chunk->state_word_ = desired;
    return true;
  }
  chunk->Reset();
  return false;
}

bool SharedRingBuffer::ReleaseChunkAsComplete(Chunk* chunk,
                                              uint8_t added_flags) {
  PERFETTO_DCHECK(chunk && chunk->is_valid() && chunk->is_being_written());
  PERFETTO_DCHECK(!(added_flags & static_cast<uint8_t>(kFlagAcquiredForWriting |
                                                       kFlagNeedsRewrite)));

  for (;;) {
    // A chunk taken back to append must come back bigger. That growth is what
    // makes the new state word differ from the one a reader may be copying
    // under, and what bounds how often that reader has to start over.
    // Publishing a freshly claimed chunk with nothing in it is merely wasteful.
    PERFETTO_DCHECK(chunk->committed_payload_size() == 0 ||
                    chunk->payload_used_ > chunk->committed_payload_size());

    ChunkHeader complete_header = ChunkHeader::FromStateWord(
        chunk->state_word_, LoadChunkTargetBuffer(chunk->begin_));
    complete_header.payload_size = chunk->payload_used_;
    complete_header.flags = static_cast<uint8_t>(
        (complete_header.flags &
         ~static_cast<uint8_t>(kFlagAcquiredForWriting | kFlagNeedsRewrite)) |
        added_flags);
    const uint32_t complete_state = complete_header.ToStateWord();

    auto* const state = state_word(chunk->begin_);
    uint32_t expected = chunk->state_word_;
    // release publishes the target buffer and the fragment records to the
    // reader's acquire load of this complete state.
    if (state->compare_exchange_strong(expected, complete_state,
                                       std::memory_order_release,
                                       std::memory_order_acquire)) {
      chunk->state_word_ = complete_state;
      return true;
    }

    const uint32_t rewrite_state = chunk->state_word_ | kFlagNeedsRewrite;
    if (PERFETTO_UNLIKELY(expected != rewrite_state)) {
      // The reader marking us kFlagNeedsRewrite is the only legitimate reason
      // for that CAS to fail. Anything else means the shared region is
      // corrupted. Do not take the process down over it: once this ring is a
      // memfd shared with traced, a CHECK here would let the other side crash
      // producers.
      PERFETTO_DFATAL("Tracing v2 chunk state corrupted: expected=%x actual=%x",
                      chunk->state_word_, expected);
      stats_.malformed_chunks.fetch_add(1, std::memory_order_relaxed);
      chunk->Reset();
      return false;
    }

    stats_.chunks_rewritten.fetch_add(1, std::memory_order_relaxed);
    std::array<uint8_t, kChunkPayloadSize> payload{};
    const uint32_t payload_used = chunk->payload_used_;
    memcpy(payload.data(), chunk->payload_begin(), payload_used);

    // Park the payload and free the marked slot before looking for a new one.
    // The reader is already past this position so this gives back no reservable
    // capacity, but leaving the slot occupied would make the next writer that
    // laps onto it burn a position it cannot use.
    state->store(kFreeStateWord, std::memory_order_release);
    chunk->Reset();

    *chunk = TryAcquireChunkForWriting(complete_header);
    if (!chunk->is_valid()) {
      stats_.chunks_lost.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    memcpy(chunk->payload_begin(), payload.data(), payload_used);
    chunk->payload_used_ = static_cast<uint8_t>(payload_used);
    // Whatever the previous lap left after |payload_used| stays there. The
    // loop's next iteration republishes that exact length, and the reader
    // never looks past it.
    //
    // The reader can collide with the relocated chunk too. Retrying is finite
    // per collision and never waits for the preempted reader.
  }
}

void SharedRingBuffer::NotifyReaderProgress() {
#if PERFETTO_TRACING_V2_HAS_FUTEX()
  RingBufferHeader* const header = ring_buffer_header();
  header->reader_generation.fetch_add(1, std::memory_order_release);

  // Bump the generation first, then look for waiters; WaitForReaderProgress()
  // does the mirror image (register, then wait on the generation it sampled
  // earlier). This is an intentional use of seq_cst between two independent
  // atomics, not the default ordering for the ring. Release/acquire alone is
  // insufficient: a load synchronizes only if it observes the other side's
  // release, and both observations are otherwise allowed to see old values.
  // For example, without the fences the following outcome is allowed:
  //
  //   writer: publishes num_writers_waiting = 1
  //   reader: publishes reader_generation = G + 1
  //   reader: still observes num_writers_waiting = 0 and skips FUTEX_WAKE
  //   writer: FUTEX_WAIT still observes G and goes to sleep
  //
  // The writer then remains asleep despite available capacity, until its wait
  // slice times out. With an unbounded wait this would be a deadlock.
  // The two seq_cst fences rule out that store-buffering outcome, so either we
  // see the waiter and wake it, or its FUTEX_WAIT sees the changed generation
  // and returns EAGAIN. Without that guarantee the reader could skip the wake
  // while the writer sleeps.
  std::atomic_thread_fence(std::memory_order_seq_cst);
  if (header->num_writers_waiting.load(std::memory_order_relaxed) == 0)
    return;  // Nobody is stalled: skip the syscall.
  FutexWakeAll(&header->reader_generation);
#endif
}

void SharedRingBuffer::WaitForReaderProgress(uint32_t last_generation,
                                             uint32_t timeout_ms) {
#if PERFETTO_TRACING_V2_HAS_FUTEX()
  RingBufferHeader* const header = ring_buffer_header();
  header->num_writers_waiting.fetch_add(1, std::memory_order_relaxed);
  // The writer half of the two-atomic missed-wakeup protocol documented in
  // NotifyReaderProgress(). acq_rel is not a drop-in replacement for this
  // fence; removing it requires redesigning the registration/recheck protocol.
  std::atomic_thread_fence(std::memory_order_seq_cst);
  FutexWait(&header->reader_generation, last_generation, timeout_ms);
  header->num_writers_waiting.fetch_sub(1, std::memory_order_relaxed);
#else
  // Unreachable: kHasFutex is false here, so UseTracingV2InProcess() never
  // turns the mechanism on and no writer can reach a stall. A DFATAL would
  // compile out in release and leave TryAcquireChunk() spinning on a wait that
  // returns instantly, so fail loudly instead.
  base::ignore_result(last_generation, timeout_ms);
  PERFETTO_FATAL("Tracing v2 reached the futex path on an unsupported OS");
#endif
}

void SharedRingBuffer::AdvanceReadPos() {
  ++read_pos_;
  // release makes all accesses to the old physical slot happen-before a
  // writer observes capacity and claims that slot in a later lap.
  ring_buffer_header()->read_pos.store(read_pos_, std::memory_order_release);
}

SharedRingBuffer::ReadResult SharedRingBuffer::TryReadChunk(ChunkHeader* header,
                                                            uint8_t* payload) {
  PERFETTO_DCHECK(header && payload);
  PERFETTO_DCHECK(read_pos_ == ring_buffer_header()->read_pos.load(
                                   std::memory_order_relaxed));

  // acquire observes FIFO reservations. Payload visibility comes from the
  // separate state-word claim below.
  const uint32_t writer_pos =
      ring_buffer_header()->write_pos.load(std::memory_order_acquire);
  if (read_pos_ == writer_pos)
    return ReadResult::kNoData;

  uint8_t* const chunk = chunk_at(read_pos_);
  auto* const state = state_word(chunk);
  uint32_t word = state->load(std::memory_order_acquire);

  for (;;) {
    if (word == kFreeStateWord) {
      // The reservation is visible but its writer has not claimed the slot.
      // Tombstoning wins against that claim or retries with the winner's state.
      if (!state->compare_exchange_strong(word, kInvalidatedChunkHeader,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
        continue;
      }
      stats_.chunks_invalidated.fetch_add(1, std::memory_order_relaxed);
      break;
    }

    if (word == kInvalidatedChunkHeader) {
      // An invalidated slot from an earlier lap whose writer has not come back
      // to free it. Leave it in place: the next writer that laps onto this slot
      // clears it, which keeps the "who frees it" rule in exactly one place.
      stats_.chunks_invalidated.fetch_add(1, std::memory_order_relaxed);
      break;
    }

    // A writer is inside this chunk. Never wait for it - it can be descheduled
    // indefinitely - so hand it homework instead: its release CAS fails, it
    // relocates the payload to a later position and frees this slot.
    if (word & kFlagAcquiredForWriting) {
      if (!(word & kFlagNeedsRewrite) &&
          !state->compare_exchange_strong(word, word | kFlagNeedsRewrite,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
        continue;
      }
      break;
    }

    // Complete. Decode the state word alone first: until the control byte says
    // this is the layout we know, the remaining header bytes mean nothing.
    ChunkHeader observed =
        ChunkHeader::FromStateWord(word, /*target_buffer=*/0);
    // A newer producer, whose payload we cannot even locate.
    const bool unsupported =
        observed.extended_header || observed.version != kChunkVersion;
    // Control state that cannot legitimately exist: writer id 0 is reserved,
    // the payload cannot be longer than the chunk, and kFlagNeedsRewrite on a
    // complete chunk has no owner left to act on it.
    const bool malformed = observed.writer_id == 0 ||
                           observed.payload_size > kChunkPayloadSize ||
                           (observed.flags & kFlagNeedsRewrite) != 0;
    // TraceWriterV2 emits at least a one-byte fragment header, even for an
    // empty packet. A zero-payload complete chunk is tolerated for now because
    // the producer ABI has not forbidden it.
    // TODO(sashwinbalaji): decide whether to reject zero-payload complete
    // chunks before the producer/service ABI is frozen.
    if (PERFETTO_UNLIKELY(unsupported || malformed)) {
      // Dropped either way, so that the ring keeps flowing, but they are not
      // the same event: one is somebody else being ahead of us, the other is
      // our own bug or a corrupted shared region.
      if (!state->compare_exchange_strong(word, kFreeStateWord,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
        continue;
      }
      if (unsupported) {
        stats_.chunks_unsupported.fetch_add(1, std::memory_order_relaxed);
      } else {
        PERFETTO_DFATAL("Tracing v2 chunk header unusable: state=%x", word);
        stats_.malformed_chunks.fetch_add(1, std::memory_order_relaxed);
      }
      break;
    }
    // Both this and the payload below were release-published by the writer's
    // transition to the complete state we acquire-loaded, so they are only
    // safe to read from here on.
    observed.target_buffer = LoadChunkTargetBuffer(chunk);

    // Copy first, then prove the copy was of a whole chunk by freeing the
    // exact state it was taken from. The owning writer may be appending after
    // payload_size while this runs - disjoint bytes - but it cannot publish
    // without changing the word, so a CAS failure means "somebody added data,
    // start over" and never "the bytes you copied were half-written".
    memcpy(payload, chunk + kChunkHeaderSize, observed.payload_size);
    if (!state->compare_exchange_strong(word, kFreeStateWord,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
      stats_.chunks_recopied.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    *header = observed;
    AdvanceReadPos();
    return ReadResult::kChunkRead;
  }

  AdvanceReadPos();
  return ReadResult::kChunkSkipped;
}

}  // namespace tracing_v2
}  // namespace perfetto
