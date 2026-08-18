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

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/paged_memory.h"
#include "src/tracing/v2/tracing_v2_abi.h"

// Step 1 supports the writer-blocks-on-a-full-ring direction of the wait
// protocol on Linux and Android only. Everywhere else the ring still builds and
// works; a writer that would have blocked is told the wait is unavailable and
// falls back to its drop behaviour.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX_BUT_NOT_QNX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#define PERFETTO_TRACING_V2_HAS_FUTEX() 1
#else
#define PERFETTO_TRACING_V2_HAS_FUTEX() 0
#endif

namespace perfetto::tracing_v2 {

// Whether this build can block a writer on a full ring. Callers outside this
// file should test this rather than the macro above, so that the platform gate
// reads as an ordinary condition.
constexpr bool kHasFutex = PERFETTO_TRACING_V2_HAS_FUTEX();

// The producer-local multi-producer/single-consumer ring.
//
// This class owns the mapping and is the *only* place that touches a chunk's
// atomic state word. Every legal transition is one method below, each an
// exact-value compare-and-swap against a word the caller observed, with the
// memory order it needs. Keeping them together is deliberate: the
// three transitions that produce a FreeForWrap word are the ones that hand a
// physical chunk to a future traversal, and it has to be auditable in one place
// that only the reader ever calls them.
//
// Everything above this class - which fragments live where, what the bytes
// mean, when to drain - is somebody else's job. See RingWriter and ChunkReader.
//
// Threading: writer-side methods are called concurrently from any number of
// writer threads. Reader-side methods are called from the single consumer.
class SharedRingBuffer {
 public:
  // |num_chunks| must be a power of two in [2, 2^30] and |chunk_size| a power
  // of two in [256, 32768].
  // Returns nullptr if they do not, or if the mapping could not be allocated.
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
  uint32_t chunk_bits() const { return chunk_bits_; }
  uint32_t fragment_size_width() const { return fragment_size_width_; }

  uint8_t* chunk_at(uint32_t chunk_index) {
    PERFETTO_DCHECK(chunk_index < num_chunks_);
    return chunks_ + static_cast<size_t>(chunk_index) * chunk_size_;
  }

  // -------------------------------------------------------------------------
  // Writer side: reserving a logical position.
  // -------------------------------------------------------------------------

  enum class ReserveOutcome {
    kReserved,
    // num_chunks positions are already outstanding. Nothing was reserved and
    // no hole was created; a stalling policy may wait and try again.
    kFull,
  };

  struct Reservation {
    ReserveOutcome outcome = ReserveOutcome::kFull;
    // Valid only when |outcome| is kReserved. This is a ticket, not an index:
    // it names the reservation for its whole life, including across a nap.
    uint32_t position = 0;
    // The read_pos sample the capacity decision was taken against. On kFull
    // this is exactly the value WaitForReadPosChange() must be given, so that a
    // reader that frees capacity between the sample and the wait turns the wait
    // into an immediate return rather than a lost wakeup.
    uint32_t read_pos_sample = 0;
  };

  // Samples capacity and, if there is room, takes the next position with a CAS.
  // A lost cursor CAS is pure contention: it reserves nothing, burns nothing,
  // and is simply retried inside this call.
  Reservation ReservePosition();

  // -------------------------------------------------------------------------
  // Writer side: the four transitions a writer may perform.
  //
  // Each takes the exact word the caller observed and returns whether its
  // compare-and-swap won. A writer must never re-attempt a transition against
  // the word a failed compare-and-swap handed back: see the per-method
  // comments.
  // -------------------------------------------------------------------------

  // FreeForWrap(wrap_count(position)) -> Acquired. |acquired_word| must be an
  // Acquired word for this writer with zero fragments.
  //
  // A reserved position gets exactly one claim attempt, because there is
  // exactly one word that reservation is entitled to swap against. On failure
  // the position is a hole that only the reader can resolve; the caller must
  // discard the observed word and, if it still wants a chunk, reserve a *later*
  // position.
  bool TryClaim(uint32_t position, uint32_t acquired_word);

  // Acquired -> Complete. On failure |*observed| receives the current word,
  // which the caller must check is a RewriteRequested for this exact chunk:
  // that is the only legal competing transition, and anything else is a
  // writer-side protocol bug.
  bool TryPublish(uint32_t chunk_index,
                  uint32_t* observed,
                  uint32_t complete_word);

  // Complete -> Acquired, for a writer taking its own cached chunk back to
  // append more fragments. Failure means the reader reclaimed it first; the
  // writer just drops its handle.
  bool TryReuse(uint32_t chunk_index, uint32_t observed);

  // RewriteRequested -> Acknowledged: "every access I made under the old
  // ownership is behind me". Only this writer may leave that state, so failure
  // is a protocol error, not a race.
  bool TryAcknowledge(uint32_t chunk_index, uint32_t observed);

  // -------------------------------------------------------------------------
  // Reader side.
  // -------------------------------------------------------------------------

  // The reader's dispatch load. Acquire, so that every fragment and size entry
  // published by the writer's release transition is visible before the walk.
  uint32_t LoadStateAcquire(uint32_t chunk_index) const;

  uint32_t LoadWritePosRelaxed() const;

  // Acquired -> RewriteRequested, passing format, flags, num_fragments and the
  // WriterID through untouched. On failure |*observed| receives the current
  // word so the reader can redispatch on it.
  bool TryMarkForRewrite(uint32_t chunk_index, uint32_t* observed);

  // The three transitions that expose a chunk to a future traversal. Only the
  // reader may perform them, and the wrap they stamp always comes from the
  // position being resolved - never from the value found in the chunk, which
  // may belong to a much older traversal.

  // FreeForWrap(wrap_count(position)) -> FreeForWrap(next_wrap(position)):
  // nobody claimed this position. One compare-and-swap both consumes the hole
  // and prepares the chunk for the writer holding position + num_chunks.
  bool TryAdvanceUnclaimed(uint32_t position, uint32_t* observed);

  // Complete -> FreeForWrap(next_wrap(position)).
  bool TryReclaimComplete(uint32_t position, uint32_t* observed);

  // Acknowledged -> FreeForWrap(next_wrap(position)). Only the reader may leave
  // Acknowledged, so failure is a protocol error.
  bool TryReclaimAcknowledged(uint32_t position);

  // -------------------------------------------------------------------------
  // Backpressure: the writer's full-ring path.
  //
  // Writers wait on read_pos; the reader publishes a new read_pos and wakes
  // them. The waiter count is a syscall-elision hint and never a predicate for
  // sleeping or for progress.
  // -------------------------------------------------------------------------

  enum class WaitOutcome {
    // read_pos changed, or the wait was interrupted, or the wake was spurious.
    // In every case the caller must re-evaluate the real capacity predicate.
    kMayHaveProgressed,
    kTimedOut,
    // Waiting is not something this caller can do: either the build has no
    // futex, or the kernel refused the wait for a reason the protocol has no
    // answer for. Either way a stalling caller must fall back to dropping
    // rather than ask again, because asking again would spin on the syscall.
    kWaitUnavailable,
  };

  // Blocks until read_pos moves away from |expected_read_pos| - which must be
  // the exact sample the Full decision was made against - or |timeout_ms|
  // elapses.
  WaitOutcome WaitForReadPosChange(uint32_t expected_read_pos,
                                   uint32_t timeout_ms);

  // The whole errno policy of the wait, in one place and with no side effects
  // so that every branch of it can be checked without making a syscall fail on
  // purpose. |wait_errno| is errno after a futex wait returned non-zero.
  static WaitOutcome WaitOutcomeForWaitErrno(int wait_errno);

  // Publishes the reader's new cursor and wakes any waiting writer. Called once
  // per drain pass, not once per reclaimed chunk.
  void PublishReadPos(uint32_t read_pos);

  // Diagnostics only. Never a correctness input.
  uint32_t num_writers_waiting_for_testing() const;
  uint32_t read_pos_for_testing() const;

  // Forces a chunk into a word the legal transitions cannot produce, which is
  // the only way to reach the reader's corruption and unknown-ABI paths.
  void SetStateWordForTesting(uint32_t chunk_index, uint32_t state_word);

  // Forces write_pos to a value ReservePosition() cannot produce, which is the
  // only way to reach the reader's forged-cursor path from inside one process.
  void SetWritePosForTesting(uint32_t write_pos);

  // Seeds both cursors to |position| and stamps every chunk with the wrap count
  // the next position mapping to it expects, which is the state a ring that had
  // really been running that long would be in. A test uses it to start next to
  // the uint32_t rollover instead of making four billion reservations.
  void SetCursorsForTesting(uint32_t position);

 private:
  // The ring control header. Its contents are left to the implementation, so
  // the layout is spelled out here rather than hidden
  // behind an anonymous pad array, and every part of it a future cross-process
  // mapping would have to agree on is asserted below.
  //
  // The two cursors get a cache line each because independent cores write them
  // continuously: read_pos is the reader's, write_pos and the waiter hint are
  // the writers'. That is about cache-line bouncing and has nothing to do with
  // atomic width - the handoff itself is a 32-bit CAS on the chunk word.
  static constexpr size_t kCacheLineSize = 64;
  static constexpr size_t kWordsPerCacheLine =
      kCacheLineSize / sizeof(uint32_t);

  struct alignas(kCacheLineSize) ReaderCursorLine {
    // Also the futex word writers park on.
    std::atomic<uint32_t> read_pos;
    uint32_t reserved[kWordsPerCacheLine - 1];
  };

  struct alignas(kCacheLineSize) WriterCursorLine {
    std::atomic<uint32_t> write_pos;
    std::atomic<uint32_t> num_writers_waiting;
    uint32_t reserved[kWordsPerCacheLine - 2];
  };

  // Two further cache lines held back for ring-wide counters and for ABI
  // growth. Anything put there is diagnostic only: relaxed atomics, and never
  // an input to an ownership or capacity decision.
  static constexpr size_t kReservedControlLines = 2;

  struct RingControl {
    ReaderCursorLine reader;
    WriterCursorLine writer;
    uint32_t reserved[kReservedControlLines * kWordsPerCacheLine];
  };

  // This header is the start of the mapping, so its size is where chunk 0
  // begins. Once the mapping is shared with traced, every one of these is
  // something the two sides have to agree on, which is why they are asserted
  // rather than described.
  static constexpr size_t kRingControlSize =
      (2 + kReservedControlLines) * kCacheLineSize;

  static_assert(sizeof(ReaderCursorLine) == kCacheLineSize, "");
  static_assert(alignof(ReaderCursorLine) == kCacheLineSize, "");
  static_assert(sizeof(WriterCursorLine) == kCacheLineSize, "");
  static_assert(alignof(WriterCursorLine) == kCacheLineSize, "");
  static_assert(offsetof(RingControl, reader) == 0, "");
  static_assert(offsetof(RingControl, writer) == kCacheLineSize,
                "The two cursors must not share a cache line");
  static_assert(sizeof(RingControl) == kRingControlSize, "");
  static_assert(alignof(RingControl) == kCacheLineSize, "");

  // The cursors and the chunk state words are plain 32-bit words in the
  // mapping, addressed by both sides. An atomic that is larger than the word it
  // guards, or that needs a lock on the side, is not something a shared mapping
  // can express at all.
  static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t), "");
  static_assert(alignof(std::atomic<uint32_t>) == alignof(uint32_t), "");
  static_assert(std::atomic<uint32_t>::is_always_lock_free,
                "The chunk state word must be lock-free on every target");

  // The mapping is page-aligned, so this offset is what decides how the chunk
  // area lands. Pinning it to the smallest legal chunk rather than to the four
  // bytes a state word needs keeps every chunk at least 256-byte aligned
  // whatever the geometry, which leaves room to widen anything in the chunk
  // header later without moving the payload off a natural boundary.
  static_assert(
      kRingControlSize % kMinChunkSize == 0,
      "The chunk area must start at a multiple of the smallest chunk");
  static_assert(kMinChunkSize % kStateWordSize == 0,
                "Every chunk must start on a naturally aligned state word");

  SharedRingBuffer(base::PagedMemory mapping,
                   uint32_t num_chunks,
                   uint32_t chunk_size);

  std::atomic<uint32_t>* state_at(uint32_t chunk_index) {
    // The mapping is zero-filled, page-aligned and never contains anything but
    // these atomics and payload bytes, so the same reinterpret_cast the v1
    // SharedMemoryABI uses over its page headers applies here: the object is
    // implicitly created by the mapping and every chunk starts at a multiple of
    // chunk_size, which is a multiple of four.
    return reinterpret_cast<std::atomic<uint32_t>*>(chunk_at(chunk_index));
  }
  const std::atomic<uint32_t>* state_at(uint32_t chunk_index) const {
    return const_cast<SharedRingBuffer*>(this)->state_at(chunk_index);
  }

  // FreeForWrap(next_wrap(position)), i.e. the word the reader stamps when it
  // finishes with |position|.
  uint32_t NextFreeWordFor(uint32_t position) const {
    return MakeFreeForWrapWord(
        NextWrapCount(position, num_chunks_, chunk_bits_));
  }

  RingControl* control() {
    return reinterpret_cast<RingControl*>(mapping_start_);
  }
  const RingControl* control() const {
    return reinterpret_cast<const RingControl*>(mapping_start_);
  }

  base::PagedMemory mapping_;
  uint8_t* const mapping_start_;
  uint8_t* const chunks_;
  const uint32_t num_chunks_;
  const uint32_t chunk_size_;
  const uint32_t chunk_bits_;
  const uint32_t fragment_size_width_;
};

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_V2_SHARED_RING_BUFFER_H_
