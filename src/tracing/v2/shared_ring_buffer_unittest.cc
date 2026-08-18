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
#include <thread>
#include <vector>

#include "perfetto/ext/base/waitable_event.h"
#include "src/tracing/v2/tracing_v2_abi.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::tracing_v2 {
namespace {

constexpr uint32_t kChunkSize = 256;
constexpr WriterID kWriterA = 7;
constexpr WriterID kWriterB = 9;

uint32_t AcquiredWord(WriterID writer) {
  return MakeDataBearingWord(ChunkState::kAcquired, ChunkFormat::kTargetBuffer,
                             0, 0, writer);
}

uint32_t CompleteWord(WriterID writer, uint32_t num_fragments) {
  return MakeDataBearingWord(ChunkState::kComplete, ChunkFormat::kTargetBuffer,
                             0, num_fragments, writer);
}

// Reads a chunk's state word without going through the ring's own accessors,
// so that a test observing the ring cannot be fooled by a bug in them.
uint32_t PeekStateWord(SharedRingBuffer* ring, uint32_t chunk_index) {
  const uint8_t* chunk = ring->chunk_at(chunk_index);
  return static_cast<uint32_t>(chunk[0]) |
         (static_cast<uint32_t>(chunk[1]) << 8) |
         (static_cast<uint32_t>(chunk[2]) << 16) |
         (static_cast<uint32_t>(chunk[3]) << 24);
}

// ---------------------------------------------------------------------------
// Geometry.
// ---------------------------------------------------------------------------

TEST(SharedRingBufferTest, RejectsIllegalGeometries) {
  // num_chunks must be a power of two in [2, 2^30].
  EXPECT_EQ(SharedRingBuffer::Create(0, kChunkSize), nullptr);
  EXPECT_EQ(SharedRingBuffer::Create(1, kChunkSize), nullptr);
  EXPECT_EQ(SharedRingBuffer::Create(3, kChunkSize), nullptr);
  EXPECT_EQ(SharedRingBuffer::Create(6, kChunkSize), nullptr);
  EXPECT_EQ(SharedRingBuffer::Create(uint32_t{1} << 31, kChunkSize), nullptr);

  // chunk_size must be a power of two in [256, 32768].
  EXPECT_EQ(SharedRingBuffer::Create(4, 0), nullptr);
  EXPECT_EQ(SharedRingBuffer::Create(4, 128), nullptr);
  EXPECT_EQ(SharedRingBuffer::Create(4, 255), nullptr);
  EXPECT_EQ(SharedRingBuffer::Create(4, 384), nullptr);
  EXPECT_EQ(SharedRingBuffer::Create(4, 65536), nullptr);
}

TEST(SharedRingBufferTest, AcceptsEveryLegalChunkSize) {
  for (uint32_t chunk_size = kMinChunkSize; chunk_size <= kMaxChunkSize;
       chunk_size *= 2) {
    auto ring = SharedRingBuffer::Create(4, chunk_size);
    ASSERT_NE(ring, nullptr) << chunk_size;
    EXPECT_EQ(ring->chunk_size(), chunk_size);
    EXPECT_EQ(ring->fragment_size_width(), FragmentSizeWidth(chunk_size));
  }
  for (uint32_t num_chunks : {2u, 4u, 8u, 16u, 1024u}) {
    auto ring = SharedRingBuffer::Create(num_chunks, kChunkSize);
    ASSERT_NE(ring, nullptr) << num_chunks;
    EXPECT_EQ(ring->num_chunks(), num_chunks);
    EXPECT_EQ(ring->chunk_bits(), Log2ForPowerOfTwo(num_chunks));
  }
}

// FreeForWrap(0) is the all-zero word, so a fresh zero-filled mapping is
// already correct and the ring does not walk it at construction.
TEST(SharedRingBufferTest, FreshMappingIsFreeForWrapZeroWithNoStampingPass) {
  auto ring = SharedRingBuffer::Create(1024, kChunkSize);
  ASSERT_NE(ring, nullptr);
  for (uint32_t i = 0; i < ring->num_chunks(); ++i) {
    ASSERT_EQ(PeekStateWord(ring.get(), i), 0u) << i;
    ASSERT_EQ(StateOf(ring->LoadStateAcquire(i)), ChunkState::kFreeForWrap);
    ASSERT_EQ(WrapCountOf(ring->LoadStateAcquire(i)), 0u);
  }
  EXPECT_EQ(ring->read_pos_for_testing(), 0u);
  EXPECT_EQ(ring->LoadWritePosRelaxed(), 0u);
  EXPECT_EQ(ring->num_writers_waiting_for_testing(), 0u);
}

TEST(SharedRingBufferTest, EveryChunkStateWordIsNaturallyAligned) {
  for (uint32_t chunk_size : {256u, 512u, 4096u, 32768u}) {
    auto ring = SharedRingBuffer::Create(8, chunk_size);
    ASSERT_NE(ring, nullptr);
    for (uint32_t i = 0; i < ring->num_chunks(); ++i) {
      const auto address = reinterpret_cast<uintptr_t>(ring->chunk_at(i));
      ASSERT_EQ(address % alignof(std::atomic<uint32_t>), 0u)
          << chunk_size << "/" << i;
      // Chunks are also cache-line aligned, which is what keeps two writers on
      // adjacent chunks from sharing a line.
      ASSERT_EQ(address % 64, 0u) << chunk_size << "/" << i;
    }
  }
}

// ---------------------------------------------------------------------------
// Reserving positions.
// ---------------------------------------------------------------------------

TEST(SharedRingBufferTest, ReservationsAreConsecutiveTicketsUntilFull) {
  auto ring = SharedRingBuffer::Create(4, kChunkSize);
  ASSERT_NE(ring, nullptr);

  for (uint32_t expected = 0; expected < 4; ++expected) {
    const auto reservation = ring->ReservePosition();
    ASSERT_EQ(reservation.outcome, SharedRingBuffer::ReserveOutcome::kReserved);
    EXPECT_EQ(reservation.position, expected);
  }

  const auto full = ring->ReservePosition();
  EXPECT_EQ(full.outcome, SharedRingBuffer::ReserveOutcome::kFull);
  // Nothing was reserved, so write_pos did not move: a full ring must not burn
  // a position.
  EXPECT_EQ(ring->LoadWritePosRelaxed(), 4u);
  // The sample the decision was taken against is exactly what a stalling
  // writer has to wait on.
  EXPECT_EQ(full.read_pos_sample, 0u);
}

TEST(SharedRingBufferTest, CapacityFollowsReadPos) {
  auto ring = SharedRingBuffer::Create(2, kChunkSize);
  ASSERT_NE(ring, nullptr);

  ASSERT_EQ(ring->ReservePosition().outcome,
            SharedRingBuffer::ReserveOutcome::kReserved);
  ASSERT_EQ(ring->ReservePosition().outcome,
            SharedRingBuffer::ReserveOutcome::kReserved);
  ASSERT_EQ(ring->ReservePosition().outcome,
            SharedRingBuffer::ReserveOutcome::kFull);

  ring->PublishReadPos(1);
  const auto reservation = ring->ReservePosition();
  ASSERT_EQ(reservation.outcome, SharedRingBuffer::ReserveOutcome::kReserved);
  EXPECT_EQ(reservation.position, 2u);
  EXPECT_EQ(reservation.read_pos_sample, 1u);
}

// ---------------------------------------------------------------------------
// The writer's transitions.
// ---------------------------------------------------------------------------

TEST(SharedRingBufferTest, ClaimTakesTheChunkForTheReservedPosition) {
  auto ring = SharedRingBuffer::Create(4, kChunkSize);
  ASSERT_NE(ring, nullptr);

  const auto reservation = ring->ReservePosition();
  ASSERT_EQ(reservation.outcome, SharedRingBuffer::ReserveOutcome::kReserved);
  EXPECT_TRUE(ring->TryClaim(reservation.position, AcquiredWord(kWriterA)));
  EXPECT_EQ(PeekStateWord(ring.get(), 0), AcquiredWord(kWriterA));
}

// A writer that reserved a position and then slept wakes up expecting the free
// word for *its* traversal. Once the reader has moved the chunk on, that word
// is gone, so the late claim cannot land. This is the schedule an all-zero free
// word could not survive.
TEST(SharedRingBufferTest, StaleClaimFailsOnceTheReaderHasAdvancedTheWrap) {
  auto ring = SharedRingBuffer::Create(4, kChunkSize);
  ASSERT_NE(ring, nullptr);

  // A writer reserves position 4 (chunk 0, wrap 1) and stalls.
  for (uint32_t i = 0; i < 4; ++i)
    ASSERT_EQ(ring->ReservePosition().outcome,
              SharedRingBuffer::ReserveOutcome::kReserved);
  ring->PublishReadPos(4);
  const auto stale = ring->ReservePosition();
  ASSERT_EQ(stale.outcome, SharedRingBuffer::ReserveOutcome::kReserved);
  ASSERT_EQ(stale.position, 4u);

  // The reader resolves positions 0 to 4 as holes, so chunk 0 ends up tagged
  // for position 8's traversal.
  for (uint32_t position = 0; position <= 4; ++position) {
    const uint32_t chunk_index = ChunkIndexOfPosition(position, 4);
    uint32_t observed = ring->LoadStateAcquire(chunk_index);
    ASSERT_TRUE(ring->TryAdvanceUnclaimed(position, &observed)) << position;
  }
  ASSERT_EQ(WrapCountOf(PeekStateWord(ring.get(), 0)), 2u);

  // The stale writer's one and only claim attempt uses the operand it computed
  // back then, and it no longer matches.
  EXPECT_FALSE(ring->TryClaim(stale.position, AcquiredWord(kWriterA)));
  EXPECT_EQ(WrapCountOf(PeekStateWord(ring.get(), 0)), 2u);

  // The writer holding position 8 - the one the chunk was actually prepared
  // for - still gets in.
  EXPECT_TRUE(ring->TryClaim(8, AcquiredWord(kWriterB)));
}

TEST(SharedRingBufferTest, TwoStaleClaimantsCannotBothAdoptTheReturnedWord) {
  auto ring = SharedRingBuffer::Create(2, kChunkSize);
  ASSERT_NE(ring, nullptr);

  // Positions 0 and 2 both map to chunk 0 but belong to different traversals.
  uint32_t observed = ring->LoadStateAcquire(0);
  ASSERT_TRUE(ring->TryAdvanceUnclaimed(0, &observed));
  ASSERT_EQ(WrapCountOf(PeekStateWord(ring.get(), 0)), 1u);

  // Position 0's writer is out, and so is anyone whose ticket is not exactly
  // position 2. Only position 2 can claim what position 0 left behind.
  EXPECT_FALSE(ring->TryClaim(0, AcquiredWord(kWriterA)));
  EXPECT_FALSE(ring->TryClaim(4, AcquiredWord(kWriterA)));
  EXPECT_TRUE(ring->TryClaim(2, AcquiredWord(kWriterB)));
}

TEST(SharedRingBufferTest, PublishReuseAndReclaimAreExactValueTransitions) {
  auto ring = SharedRingBuffer::Create(4, kChunkSize);
  ASSERT_NE(ring, nullptr);

  ASSERT_EQ(ring->ReservePosition().position, 0u);
  ASSERT_TRUE(ring->TryClaim(0, AcquiredWord(kWriterA)));

  uint32_t observed = AcquiredWord(kWriterA);
  ASSERT_TRUE(ring->TryPublish(0, &observed, CompleteWord(kWriterA, 2)));
  EXPECT_EQ(PeekStateWord(ring.get(), 0), CompleteWord(kWriterA, 2));

  // A publication against a word that is no longer there fails and reports what
  // is.
  uint32_t stale = AcquiredWord(kWriterA);
  EXPECT_FALSE(ring->TryPublish(0, &stale, CompleteWord(kWriterA, 3)));
  EXPECT_EQ(stale, CompleteWord(kWriterA, 2));

  // Reuse takes the same chunk back, keeping the published count.
  ASSERT_TRUE(ring->TryReuse(0, CompleteWord(kWriterA, 2)));
  EXPECT_EQ(StateOf(PeekStateWord(ring.get(), 0)), ChunkState::kAcquired);
  EXPECT_EQ(NumFragmentsOf(PeekStateWord(ring.get(), 0)), 2u);

  observed = WithState(CompleteWord(kWriterA, 2), ChunkState::kAcquired);
  ASSERT_TRUE(ring->TryPublish(0, &observed, CompleteWord(kWriterA, 5)));

  // The reader consumes it and stamps the wrap for the *next* traversal of this
  // chunk, taken from the position it just resolved.
  observed = CompleteWord(kWriterA, 5);
  ASSERT_TRUE(ring->TryReclaimComplete(0, &observed));
  EXPECT_EQ(PeekStateWord(ring.get(), 0), MakeFreeForWrapWord(1));
}

TEST(SharedRingBufferTest, MarkForRewritePassesEveryOtherFieldThrough) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);

  const uint32_t acquired = MakeDataBearingWord(
      ChunkState::kAcquired, ChunkFormat::kReservedRouting,
      kFlagContinuesFromPrevChunk | kFlagDataLoss, 0, kWriterA);
  ASSERT_TRUE(ring->TryClaim(0, acquired));

  const uint32_t published = MakeDataBearingWord(
      ChunkState::kAcquired, ChunkFormat::kReservedRouting,
      kFlagContinuesFromPrevChunk | kFlagDataLoss, 3, kWriterA);
  // Simulate the writer having appended three fragments by reusing the chunk
  // through the publish/reuse pair.
  uint32_t observed = acquired;
  ASSERT_TRUE(ring->TryPublish(0, &observed,
                               WithState(published, ChunkState::kComplete)));
  ASSERT_TRUE(ring->TryReuse(0, WithState(published, ChunkState::kComplete)));

  observed = published;
  ASSERT_TRUE(ring->TryMarkForRewrite(0, &observed));
  const uint32_t marked = PeekStateWord(ring.get(), 0);
  EXPECT_EQ(StateOf(marked), ChunkState::kRewriteRequested);
  // The reader can arbitrate a chunk whose format it does not understand
  // precisely because it changes nothing but the state.
  EXPECT_EQ(FormatOf(marked), ChunkFormat::kReservedRouting);
  EXPECT_EQ(PayloadFlagsOf(marked),
            kFlagContinuesFromPrevChunk | kFlagDataLoss);
  EXPECT_EQ(NumFragmentsOf(marked), 3u);
  EXPECT_EQ(WriterIdOf(marked), kWriterA);
}

TEST(SharedRingBufferTest, AcknowledgeThenReclaimReturnsTheChunk) {
  auto ring = SharedRingBuffer::Create(4, kChunkSize);
  ASSERT_NE(ring, nullptr);

  ASSERT_TRUE(ring->TryClaim(0, AcquiredWord(kWriterA)));
  uint32_t observed = AcquiredWord(kWriterA);
  ASSERT_TRUE(ring->TryMarkForRewrite(0, &observed));
  const uint32_t marked =
      WithState(AcquiredWord(kWriterA), ChunkState::kRewriteRequested);
  EXPECT_EQ(PeekStateWord(ring.get(), 0), marked);

  // Nobody but the owning writer can leave RewriteRequested, and the writer
  // says nothing about who gets the chunk next.
  ASSERT_TRUE(ring->TryAcknowledge(0, marked));
  EXPECT_EQ(PeekStateWord(ring.get(), 0), kAcknowledgedWord);

  // Only the reader turns that into a free word, and it stamps the wrap of the
  // position it is resolving.
  ASSERT_TRUE(ring->TryReclaimAcknowledged(4));
  EXPECT_EQ(PeekStateWord(ring.get(), 0), MakeFreeForWrapWord(2));
}

// Every word that says "this chunk is claimable" comes out of exactly three
// methods, and all three are the reader's. A writer has no way to produce one:
// the strongest thing it can say is Acknowledged.
TEST(SharedRingBufferTest, NoWriterTransitionProducesAFreeWord) {
  auto ring = SharedRingBuffer::Create(4, kChunkSize);
  ASSERT_NE(ring, nullptr);

  ASSERT_TRUE(ring->TryClaim(0, AcquiredWord(kWriterA)));
  uint32_t observed = AcquiredWord(kWriterA);
  ASSERT_TRUE(ring->TryPublish(0, &observed, CompleteWord(kWriterA, 1)));
  EXPECT_NE(StateOf(PeekStateWord(ring.get(), 0)), ChunkState::kFreeForWrap);

  ASSERT_TRUE(ring->TryReuse(0, CompleteWord(kWriterA, 1)));
  EXPECT_NE(StateOf(PeekStateWord(ring.get(), 0)), ChunkState::kFreeForWrap);

  observed = WithState(CompleteWord(kWriterA, 1), ChunkState::kAcquired);
  ASSERT_TRUE(ring->TryMarkForRewrite(0, &observed));
  ASSERT_TRUE(ring->TryAcknowledge(
      0, WithState(CompleteWord(kWriterA, 1), ChunkState::kRewriteRequested)));
  EXPECT_NE(StateOf(PeekStateWord(ring.get(), 0)), ChunkState::kFreeForWrap);
  EXPECT_EQ(PeekStateWord(ring.get(), 0), kAcknowledgedWord);

  ASSERT_TRUE(ring->TryReclaimAcknowledged(0));
  EXPECT_EQ(StateOf(PeekStateWord(ring.get(), 0)), ChunkState::kFreeForWrap);
}

TEST(SharedRingBufferTest, ReclaimStampsTheNextWrapAcrossTheCursorRollover) {
  // With sixteen chunks the wrap count returns to zero when the 32-bit cursor
  // rolls over, long before the 29-bit field is exhausted. A reader that
  // incremented the value it found in the chunk would stamp 0x10000000 here and
  // lock out the writer holding the next position.
  auto ring = SharedRingBuffer::Create(16, kChunkSize);
  ASSERT_NE(ring, nullptr);

  const uint32_t kLastLap = 0xfffffff0u;  // chunk 0, wrap 0x0fffffff
  ASSERT_EQ(ChunkIndexOfPosition(kLastLap, 16), 0u);
  ASSERT_EQ(WrapCountOfPosition(kLastLap, ring->chunk_bits()), 0x0fffffffu);

  // Walk chunk 0 into Acknowledged, which is the state the reader reclaims.
  ASSERT_TRUE(ring->TryClaim(0, AcquiredWord(kWriterA)));
  uint32_t observed = AcquiredWord(kWriterA);
  ASSERT_TRUE(ring->TryMarkForRewrite(0, &observed));
  ASSERT_TRUE(ring->TryAcknowledge(
      0, WithState(AcquiredWord(kWriterA), ChunkState::kRewriteRequested)));

  ASSERT_TRUE(ring->TryReclaimAcknowledged(kLastLap));
  EXPECT_EQ(PeekStateWord(ring.get(), 0), MakeFreeForWrapWord(0));
  // The writer holding position 0, the one that follows kLastLap on chunk 0,
  // is exactly the one that can claim it.
  EXPECT_TRUE(ring->TryClaim(0, AcquiredWord(kWriterA)));
}

// ---------------------------------------------------------------------------
// Backpressure.
// ---------------------------------------------------------------------------

#if PERFETTO_TRACING_V2_HAS_FUTEX()

TEST(SharedRingBufferTest, BlockedWriterWakesWhenTheReaderReleasesCapacity) {
  auto ring = SharedRingBuffer::Create(2, kChunkSize);
  ASSERT_NE(ring, nullptr);
  ASSERT_EQ(ring->ReservePosition().outcome,
            SharedRingBuffer::ReserveOutcome::kReserved);
  ASSERT_EQ(ring->ReservePosition().outcome,
            SharedRingBuffer::ReserveOutcome::kReserved);

  const auto full = ring->ReservePosition();
  ASSERT_EQ(full.outcome, SharedRingBuffer::ReserveOutcome::kFull);

  base::WaitableEvent about_to_wait;
  std::atomic<bool> reserved{false};
  std::thread writer([&] {
    about_to_wait.Notify();
    // Bounded only as a deadlock guard: the reader below always frees capacity,
    // so a correct implementation never comes close to the timeout.
    const auto outcome =
        ring->WaitForReadPosChange(full.read_pos_sample, 30000);
    EXPECT_EQ(outcome, SharedRingBuffer::WaitOutcome::kMayHaveProgressed);
    reserved.store(ring->ReservePosition().outcome ==
                   SharedRingBuffer::ReserveOutcome::kReserved);
  });

  about_to_wait.Wait();
  // Spin until the writer is actually parked, so that this exercises the wake
  // rather than the early-return path.
  while (ring->num_writers_waiting_for_testing() == 0)
    std::this_thread::yield();

  ring->PublishReadPos(1);
  writer.join();
  EXPECT_TRUE(reserved.load());
  EXPECT_EQ(ring->num_writers_waiting_for_testing(), 0u);
}

// If the cursor moves between the capacity sample and the wait, the wait must
// come straight back rather than park on a value that will never be stored
// again. Without that the writer sleeps with space available.
TEST(SharedRingBufferTest, WaitReturnsImmediatelyIfTheCursorAlreadyMoved) {
  auto ring = SharedRingBuffer::Create(2, kChunkSize);
  ASSERT_NE(ring, nullptr);

  const uint32_t stale_sample = ring->read_pos_for_testing();
  ring->PublishReadPos(5);

  const auto outcome = ring->WaitForReadPosChange(stale_sample, 30000);
  EXPECT_EQ(outcome, SharedRingBuffer::WaitOutcome::kMayHaveProgressed);
  EXPECT_EQ(ring->num_writers_waiting_for_testing(), 0u);
}

TEST(SharedRingBufferTest, WaitTimesOutWithoutAWake) {
  auto ring = SharedRingBuffer::Create(2, kChunkSize);
  ASSERT_NE(ring, nullptr);
  // 1 ms because this test relies on the timeout firing.
  EXPECT_EQ(ring->WaitForReadPosChange(0, 1),
            SharedRingBuffer::WaitOutcome::kTimedOut);
  EXPECT_EQ(ring->num_writers_waiting_for_testing(), 0u);
}

TEST(SharedRingBufferTest, SeveralWaitersAreAllReleased) {
  constexpr uint32_t kNumWaiters = 8;
  auto ring = SharedRingBuffer::Create(2, kChunkSize);
  ASSERT_NE(ring, nullptr);

  std::atomic<uint32_t> woke{0};
  std::vector<std::thread> waiters;
  for (uint32_t i = 0; i < kNumWaiters; ++i) {
    waiters.emplace_back([&] {
      ring->WaitForReadPosChange(0, 30000);
      woke.fetch_add(1);
    });
  }

  while (ring->num_writers_waiting_for_testing() < kNumWaiters)
    std::this_thread::yield();

  ring->PublishReadPos(1);
  for (auto& waiter : waiters)
    waiter.join();

  EXPECT_EQ(woke.load(), kNumWaiters);
  EXPECT_EQ(ring->num_writers_waiting_for_testing(), 0u);
}

// The hint exists only to elide a syscall. A reader that never looks at it is
// slower, not wrong, so a writer must make progress even then.
TEST(SharedRingBufferTest, WaiterHintIsNotRequiredForProgress) {
  auto ring = SharedRingBuffer::Create(2, kChunkSize);
  ASSERT_NE(ring, nullptr);
  EXPECT_EQ(ring->num_writers_waiting_for_testing(), 0u);

  // Publishing with no waiters must not wake anything or leave the hint dirty.
  ring->PublishReadPos(3);
  EXPECT_EQ(ring->num_writers_waiting_for_testing(), 0u);
  EXPECT_EQ(ring->read_pos_for_testing(), 3u);
}

#endif  // PERFETTO_TRACING_V2_HAS_FUTEX()

// The errno policy of the wait, checked directly rather than by arranging for
// the kernel to fail. Every branch matters to a stalling writer: two of them
// send it back to the capacity predicate, one is the slice expiring, and the
// rest have to stop it waiting at all.
TEST(SharedRingBufferTest, WaitErrnoPolicy) {
  using WaitOutcome = SharedRingBuffer::WaitOutcome;
  EXPECT_EQ(SharedRingBuffer::WaitOutcomeForWaitErrno(ETIMEDOUT),
            WaitOutcome::kTimedOut);
  // The cursor moved between the load and the syscall.
  EXPECT_EQ(SharedRingBuffer::WaitOutcomeForWaitErrno(EAGAIN),
            WaitOutcome::kMayHaveProgressed);
  // A signal. Deliberately not retried inside the wrapper.
  EXPECT_EQ(SharedRingBuffer::WaitOutcomeForWaitErrno(EINTR),
            WaitOutcome::kMayHaveProgressed);
  // Anything else means this address cannot be waited on, so a stalling writer
  // must drop rather than come straight back into the same failing syscall.
  EXPECT_EQ(SharedRingBuffer::WaitOutcomeForWaitErrno(EINVAL),
            WaitOutcome::kWaitUnavailable);
  EXPECT_EQ(SharedRingBuffer::WaitOutcomeForWaitErrno(EFAULT),
            WaitOutcome::kWaitUnavailable);
  EXPECT_EQ(SharedRingBuffer::WaitOutcomeForWaitErrno(ENOSYS),
            WaitOutcome::kWaitUnavailable);
}

// ---------------------------------------------------------------------------
// Control-block layout.
// ---------------------------------------------------------------------------

// The static_asserts in the header pin the layout at compile time. This checks
// the consequence a second process would actually see: the chunk area starts
// after a control block of one fixed size whatever the geometry, and every
// chunk lands on a boundary its state word can be atomic on.
TEST(SharedRingBufferTest, ChunksStartAfterAFixedControlBlock) {
  for (uint32_t chunk_size : {256u, 512u, 4096u, 32768u}) {
    auto ring = SharedRingBuffer::Create(4, chunk_size);
    ASSERT_NE(ring, nullptr);
    const uintptr_t first = reinterpret_cast<uintptr_t>(ring->chunk_at(0));
    const uintptr_t last = reinterpret_cast<uintptr_t>(ring->chunk_at(3));
    // The mapping is page-aligned and the control block is a whole number of
    // minimum chunks, so this holds for every legal chunk_size.
    EXPECT_EQ(first % kMinChunkSize, 0u) << "chunk_size " << chunk_size;
    EXPECT_EQ(last - first, 3u * chunk_size);
  }
}

}  // namespace
}  // namespace perfetto::tracing_v2
