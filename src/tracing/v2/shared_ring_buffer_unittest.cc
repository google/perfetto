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
#include <thread>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/waitable_event.h"
#include "src/tracing/v2/tracing_v2_abi.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::tracing_v2 {
namespace {

constexpr uint32_t kChunkSize = 256;
constexpr WriterID kWriterA = 7;
constexpr WriterID kWriterB = 9;

uint32_t BeingWrittenWord(WriterID writer) {
  return MakeDataStateWord(ChunkState::kBeingWritten,
                           ChunkFormat::kTargetBuffer, 0, 0, writer);
}

uint32_t CompleteWord(WriterID writer, uint32_t num_fragments) {
  return MakeDataStateWord(ChunkState::kComplete, ChunkFormat::kTargetBuffer, 0,
                           num_fragments, writer);
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

// Spins until |condition| holds. Bounded: returns false after a deadline no
// correct implementation comes anywhere near, so a broken wait path fails the
// test instead of hanging the process. A caller must still unblock and join
// every thread it spawned before asserting on the result.
template <typename ConditionFn>
bool SpinUntil(ConditionFn condition) {
  const base::TimeMillis deadline =
      base::GetWallTimeMs() + base::TimeMillis(30000);
  while (base::GetWallTimeMs() < deadline) {
    if (condition())
      return true;
    std::this_thread::yield();
  }
  return false;
}

// ---------------------------------------------------------------------------
// Ring dimensions.
// ---------------------------------------------------------------------------

TEST(SharedRingBufferTest, RejectsInvalidChunkCountsAndSizes) {
  // num_chunks must be a power of two no larger than 2^30.
  EXPECT_EQ(SharedRingBuffer::Create(0, kChunkSize), nullptr);
  EXPECT_EQ(SharedRingBuffer::Create(3, kChunkSize), nullptr);
  EXPECT_EQ(SharedRingBuffer::Create(6, kChunkSize), nullptr);
  EXPECT_EQ(SharedRingBuffer::Create(uint32_t{1} << 31, kChunkSize), nullptr);

  // chunk_size must be at least 256 and keep the state word aligned.
  EXPECT_EQ(SharedRingBuffer::Create(4, 0), nullptr);
  EXPECT_EQ(SharedRingBuffer::Create(4, 128), nullptr);
  EXPECT_EQ(SharedRingBuffer::Create(4, 255), nullptr);
  EXPECT_EQ(SharedRingBuffer::Create(4, 258), nullptr);
}

TEST(SharedRingBufferTest, AcceptsValidChunkCountsAndSizes) {
  for (uint32_t chunk_size : {256u, 260u, 512u, 1000u, 4096u, 65536u}) {
    auto ring = SharedRingBuffer::Create(4, chunk_size);
    ASSERT_NE(ring, nullptr) << chunk_size;
    EXPECT_EQ(ring->chunk_size(), chunk_size);
  }
  for (uint32_t num_chunks : {1u, 2u, 4u, 8u, 16u, 1024u}) {
    auto ring = SharedRingBuffer::Create(num_chunks, kChunkSize);
    ASSERT_NE(ring, nullptr) << num_chunks;
    EXPECT_EQ(ring->num_chunks(), num_chunks);
    EXPECT_EQ(ring->chunk_index_bits(), GetChunkIndexBits(num_chunks));
  }
}

// chunk_size must keep the state word aligned and be at least 256 bytes;
// nothing requires it to be a power of two and there is no arbitrary maximum.
TEST(SharedRingBufferTest, AcceptsAlignedNonPowerOfTwoChunkSizes) {
  EXPECT_NE(SharedRingBuffer::Create(4, 260), nullptr);
  EXPECT_NE(SharedRingBuffer::Create(4, 1000), nullptr);
  EXPECT_NE(SharedRingBuffer::Create(4, 65536), nullptr);
  // Still rejected: a misaligned state word, or a chunk below the minimum.
  EXPECT_EQ(SharedRingBuffer::Create(4, 258), nullptr);
  EXPECT_EQ(SharedRingBuffer::Create(4, 255), nullptr);
}

// Free(0) is the all-zero word, so a fresh zero-filled mapping is
// already correct and the ring does not walk it at construction.
TEST(SharedRingBufferTest, FreshMappingIsFreeZeroWithNoStampingPass) {
  auto ring = SharedRingBuffer::Create(1024, kChunkSize);
  ASSERT_NE(ring, nullptr);
  for (uint32_t i = 0; i < ring->num_chunks(); ++i) {
    ASSERT_EQ(PeekStateWord(ring.get(), i), 0u) << i;
    ASSERT_EQ(ChunkStateOf(ring->LoadChunkStateWord(i)), ChunkState::kFree);
    ASSERT_EQ(WrapCountOf(ring->LoadChunkStateWord(i)), 0u);
  }
  EXPECT_EQ(ring->read_pos_for_testing(), 0u);
  EXPECT_EQ(ring->LoadWritePos(), 0u);
  EXPECT_EQ(ring->num_writers_waiting_for_testing(), 0u);
}

TEST(SharedRingBufferTest, EveryChunkStateWordIsNaturallyAligned) {
  for (uint32_t chunk_size : {256u, 260u, 512u, 4096u, 65536u}) {
    auto ring = SharedRingBuffer::Create(8, chunk_size);
    ASSERT_NE(ring, nullptr);
    for (uint32_t i = 0; i < ring->num_chunks(); ++i) {
      const auto address = reinterpret_cast<uintptr_t>(ring->chunk_at(i));
      // The ABI's alignment requirement: every state word is naturally
      // aligned, whatever the stride.
      ASSERT_EQ(address % alignof(std::atomic<uint32_t>), 0u)
          << chunk_size << "/" << i;
      // For a cache-line-multiple stride the chunks additionally stay
      // line-aligned, which keeps two writers on adjacent chunks off each
      // other's lines. That is a property of those chunk sizes, not of the
      // ABI.
      if (chunk_size % 64 == 0) {
        ASSERT_EQ(address % 64, 0u) << chunk_size << "/" << i;
      }
    }
  }
}

// The header is one 64-byte line and chunk 0 starts on the next one. The
// mapping is page-aligned, so the chunk area's misalignment from any
// page-divisor boundary is exactly the header size - which is how the header's
// size stays observable from outside the class.
TEST(SharedRingBufferTest, ChunksStartRightAfterTheOneLineHeader) {
  for (uint32_t chunk_size : {256u, 260u, 4096u}) {
    auto ring = SharedRingBuffer::Create(4, chunk_size);
    ASSERT_NE(ring, nullptr);
    const uintptr_t first = reinterpret_cast<uintptr_t>(ring->chunk_at(0));
    EXPECT_EQ(first % 256, 64u) << "chunk_size " << chunk_size;
    const uintptr_t last = reinterpret_cast<uintptr_t>(ring->chunk_at(3));
    EXPECT_EQ(last - first, 3u * chunk_size);
  }
}

// The whole mapping-size policy, branch by branch, without asking the
// allocator for anything.
TEST(SharedRingBufferTest, MappingSizeArithmeticIsChecked) {
  constexpr size_t kPage = 4096;
  // The nominal case is the header plus the chunk area, exactly.
  EXPECT_EQ(SharedRingBuffer::ComputeAllocationSizeForTesting(4, 256, kPage),
            std::make_optional<size_t>(64 + 4 * 256));

  // The largest legal ring is representable on a 64-bit host and nothing
  // smaller can overflow there; on a 32-bit host the same request must be
  // rejected by arithmetic, not by a crash inside the allocator.
  const auto largest = SharedRingBuffer::ComputeAllocationSizeForTesting(
      1u << 30, 0xfffffffcu, kPage);
  if (sizeof(size_t) >= 8) {
    ASSERT_TRUE(largest.has_value());
    EXPECT_EQ(*largest, 64 + (uint64_t{1} << 30) * 0xfffffffcull);
  } else {
    EXPECT_FALSE(largest.has_value());
  }

  // The guard-space headroom: a total that fits in size_t but whose padded
  // allocator request would not is rejected here, because
  // PagedMemory::Allocate() does its rounding and guard-page addition in
  // unchecked size_t arithmetic.
  EXPECT_FALSE(
      SharedRingBuffer::ComputeAllocationSizeForTesting(4, 256, SIZE_MAX / 3)
          .has_value());
  EXPECT_FALSE(
      SharedRingBuffer::ComputeAllocationSizeForTesting(4, 256, SIZE_MAX / 2)
          .has_value());
}

// ---------------------------------------------------------------------------
// Reserving positions.
// ---------------------------------------------------------------------------

TEST(SharedRingBufferTest, ReservationsAreConsecutiveTicketsUntilFull) {
  auto ring = SharedRingBuffer::Create(4, kChunkSize);
  ASSERT_NE(ring, nullptr);

  for (uint32_t expected = 0; expected < 4; ++expected) {
    const auto reservation = ring->TryReserveWritePos();
    ASSERT_EQ(reservation.result, SharedRingBuffer::ReserveResult::kReserved);
    EXPECT_EQ(reservation.position, expected);
  }

  const auto full = ring->TryReserveWritePos();
  EXPECT_EQ(full.result, SharedRingBuffer::ReserveResult::kFull);
  // Nothing was reserved, so write_pos did not move: a full ring must not burn
  // a position.
  EXPECT_EQ(ring->LoadWritePos(), 4u);
  // The sample the decision was taken against is exactly what a stalling
  // writer has to wait on.
  EXPECT_EQ(full.read_pos_sample, 0u);
}

TEST(SharedRingBufferTest, CapacityFollowsReadPos) {
  auto ring = SharedRingBuffer::Create(2, kChunkSize);
  ASSERT_NE(ring, nullptr);

  ASSERT_EQ(ring->TryReserveWritePos().result,
            SharedRingBuffer::ReserveResult::kReserved);
  ASSERT_EQ(ring->TryReserveWritePos().result,
            SharedRingBuffer::ReserveResult::kReserved);
  ASSERT_EQ(ring->TryReserveWritePos().result,
            SharedRingBuffer::ReserveResult::kFull);

  // Publishing read_pos must preserve a concurrently updated write_pos.
  ring->PublishReadPos(1);
  EXPECT_EQ(ring->read_pos_for_testing(), 1u);
  EXPECT_EQ(ring->LoadWritePos(), 2u);

  const auto reservation = ring->TryReserveWritePos();
  ASSERT_EQ(reservation.result, SharedRingBuffer::ReserveResult::kReserved);
  EXPECT_EQ(reservation.position, 2u);
  EXPECT_EQ(reservation.read_pos_sample, 1u);
  EXPECT_EQ(ring->read_pos_for_testing(), 1u);
  EXPECT_EQ(ring->LoadWritePos(), 3u);
}

// One load of the packed positions decides capacity across uint32_t rollover.
TEST(SharedRingBufferTest, CapacityDecisionsAreExactAcrossPositionRollover) {
  auto ring = SharedRingBuffer::Create(8, kChunkSize);
  ASSERT_NE(ring, nullptr);
  const uint32_t kSeed = 0xfffffffcu;  // Four positions before the rollover.
  ring->SetPositionsForTesting(kSeed);

  for (uint32_t i = 0; i < 8; ++i) {
    const auto reservation = ring->TryReserveWritePos();
    ASSERT_EQ(reservation.result, SharedRingBuffer::ReserveResult::kReserved)
        << i;
    EXPECT_EQ(reservation.position, kSeed + i) << i;
  }
  // write_pos has wrapped: 0xfffffffc + 8 = 4.
  EXPECT_EQ(ring->LoadWritePos(), 4u);

  const auto full = ring->TryReserveWritePos();
  ASSERT_EQ(full.result, SharedRingBuffer::ReserveResult::kFull);
  EXPECT_EQ(full.read_pos_sample, kSeed);

  ring->PublishReadPos(kSeed + 1);
  const auto reservation = ring->TryReserveWritePos();
  ASSERT_EQ(reservation.result, SharedRingBuffer::ReserveResult::kReserved);
  EXPECT_EQ(reservation.position, 4u);
  EXPECT_EQ(reservation.read_pos_sample, kSeed + 1);
}

// ---------------------------------------------------------------------------
// The two read/write-position compare-and-swap conflicts, forced
// deterministically.
//
// Two racing threads cannot be scheduled into "load, then lose the CAS" on
// demand. These tests start the production loop with an old rw_positions
// value. The first compare-and-swap must fail, and its expected argument is
// replaced with the current value. The test then checks that the retry uses
// both halves of that current value.
// ---------------------------------------------------------------------------

// A reader publication lands between the writer's load and its CAS. The
// reservation must recheck capacity with the value returned by the failed CAS,
// not reuse either half of the old value.
TEST(SharedRingBufferTest, ReservationCasLosesToPublicationAndRedispatches) {
  auto ring = SharedRingBuffer::Create(4, kChunkSize);
  ASSERT_NE(ring, nullptr);
  ASSERT_EQ(ring->TryReserveWritePos().position, 0u);
  ASSERT_EQ(ring->TryReserveWritePos().position, 1u);

  // The value the writer loaded before the reader published.
  const uint64_t stale_rw_positions = PackRwPositions(2, 0);
  ring->PublishReadPos(1);  // Now (write=2, read=1).

  const auto reservation =
      ring->TryReserveWritePosForTesting(stale_rw_positions);
  ASSERT_EQ(reservation.result, SharedRingBuffer::ReserveResult::kReserved);
  EXPECT_EQ(reservation.position, 2u);
  // The failed CAS returned read_pos=1. Seeing that value here proves that the
  // retry did not keep read_pos=0 from the old value.
  EXPECT_EQ(reservation.read_pos_sample, 1u);
  EXPECT_EQ(ring->LoadWritePos(), 3u);
  EXPECT_EQ(ring->read_pos_for_testing(), 1u);
}

// The same losing CAS, but the value returned by the failure says that the
// ring is full. The retry must take the Full exit, without burning a position.
TEST(SharedRingBufferTest, ReservationCasLossRedispatchesIntoFull) {
  auto ring = SharedRingBuffer::Create(2, kChunkSize);
  ASSERT_NE(ring, nullptr);
  ASSERT_EQ(ring->TryReserveWritePos().position, 0u);

  const uint64_t stale_rw_positions =
      PackRwPositions(1, 0);  // One outstanding: capacity left.
  ASSERT_EQ(ring->TryReserveWritePos().position, 1u);  // Now (2, 0): full.

  const auto reservation =
      ring->TryReserveWritePosForTesting(stale_rw_positions);
  EXPECT_EQ(reservation.result, SharedRingBuffer::ReserveResult::kFull);
  EXPECT_EQ(reservation.read_pos_sample, 0u);
  EXPECT_EQ(ring->LoadWritePos(), 2u);
}

// A writer reservation lands between the reader's load and its CAS. The
// publication retry must preserve the returned write_pos: had it kept the
// stale half, the reservation would be erased and write_pos would read 0.
TEST(SharedRingBufferTest, PublicationCasLosesToReservationAndPreservesIt) {
  auto ring = SharedRingBuffer::Create(4, kChunkSize);
  ASSERT_NE(ring, nullptr);

  // The value loaded by the reader before the writer reserved position 0.
  const uint64_t stale_rw_positions = PackRwPositions(0, 0);
  ASSERT_EQ(ring->TryReserveWritePos().position, 0u);  // Now (write=1, read=0).

  ring->PublishReadPosForTesting(stale_rw_positions, 1);
  EXPECT_EQ(ring->LoadWritePos(), 1u);
  EXPECT_EQ(ring->read_pos_for_testing(), 1u);
}

// The ABI accepts any power-of-two chunk count in [1, 2^30].
TEST(SharedRingBufferTest, OneChunkRingWorks) {
  auto ring = SharedRingBuffer::Create(1, kChunkSize);
  ASSERT_NE(ring, nullptr);
  EXPECT_EQ(ring->num_chunks(), 1u);
  EXPECT_EQ(ring->chunk_index_bits(), 0u);

  const auto reservation = ring->TryReserveWritePos();
  ASSERT_EQ(reservation.result, SharedRingBuffer::ReserveResult::kReserved);
  EXPECT_EQ(reservation.position, 0u);
  EXPECT_EQ(ring->TryReserveWritePos().result,
            SharedRingBuffer::ReserveResult::kFull);

  ASSERT_TRUE(ring->TryAcquireChunkForWriting(0, BeingWrittenWord(kWriterA)));
  uint32_t observed = BeingWrittenWord(kWriterA);
  ASSERT_TRUE(
      ring->TrySetChunkComplete(0, &observed, CompleteWord(kWriterA, 1)));
  observed = CompleteWord(kWriterA, 1);
  ASSERT_TRUE(ring->TryReleaseCompleteChunkAsFree(0, &observed));
  // With one chunk the wrap count advances on every position.
  EXPECT_EQ(PeekStateWord(ring.get(), 0), MakeFreeStateWord(1));
  ring->PublishReadPos(1);

  const auto next = ring->TryReserveWritePos();
  ASSERT_EQ(next.result, SharedRingBuffer::ReserveResult::kReserved);
  EXPECT_EQ(next.position, 1u);
  EXPECT_TRUE(ring->TryAcquireChunkForWriting(1, BeingWrittenWord(kWriterB)));
}

// ---------------------------------------------------------------------------
// The writer's transitions.
// ---------------------------------------------------------------------------

TEST(SharedRingBufferTest, ClaimTakesTheChunkForTheReservedPosition) {
  auto ring = SharedRingBuffer::Create(4, kChunkSize);
  ASSERT_NE(ring, nullptr);

  const auto reservation = ring->TryReserveWritePos();
  ASSERT_EQ(reservation.result, SharedRingBuffer::ReserveResult::kReserved);
  EXPECT_TRUE(ring->TryAcquireChunkForWriting(reservation.position,
                                              BeingWrittenWord(kWriterA)));
  EXPECT_EQ(PeekStateWord(ring.get(), 0), BeingWrittenWord(kWriterA));
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
    ASSERT_EQ(ring->TryReserveWritePos().result,
              SharedRingBuffer::ReserveResult::kReserved);
  ring->PublishReadPos(4);
  const auto stale = ring->TryReserveWritePos();
  ASSERT_EQ(stale.result, SharedRingBuffer::ReserveResult::kReserved);
  ASSERT_EQ(stale.position, 4u);

  // The reader resolves positions 0 to 4 as holes, so chunk 0 ends up tagged
  // for position 8's traversal.
  for (uint32_t position = 0; position <= 4; ++position) {
    const uint32_t chunk_index = ChunkIndexOfPosition(position, 4);
    uint32_t observed = ring->LoadChunkStateWord(chunk_index);
    ASSERT_TRUE(ring->TryMoveFreeChunkToNextWrap(position, &observed))
        << position;
  }
  ASSERT_EQ(WrapCountOf(PeekStateWord(ring.get(), 0)), 2u);

  // The stale writer's one and only claim attempt uses the operand it computed
  // back then, and it no longer matches.
  EXPECT_FALSE(ring->TryAcquireChunkForWriting(stale.position,
                                               BeingWrittenWord(kWriterA)));
  EXPECT_EQ(WrapCountOf(PeekStateWord(ring.get(), 0)), 2u);

  // The writer holding position 8 - the one the chunk was actually prepared
  // for - still gets in.
  EXPECT_TRUE(ring->TryAcquireChunkForWriting(8, BeingWrittenWord(kWriterB)));
}

TEST(SharedRingBufferTest, TwoStaleClaimantsCannotBothAdoptTheReturnedWord) {
  auto ring = SharedRingBuffer::Create(2, kChunkSize);
  ASSERT_NE(ring, nullptr);

  // Positions 0 and 2 both map to chunk 0 but belong to different traversals.
  uint32_t observed = ring->LoadChunkStateWord(0);
  ASSERT_TRUE(ring->TryMoveFreeChunkToNextWrap(0, &observed));
  ASSERT_EQ(WrapCountOf(PeekStateWord(ring.get(), 0)), 1u);

  // Position 0's writer is out, and so is anyone whose ticket is not exactly
  // position 2. Only position 2 can claim what position 0 left behind.
  EXPECT_FALSE(ring->TryAcquireChunkForWriting(0, BeingWrittenWord(kWriterA)));
  EXPECT_FALSE(ring->TryAcquireChunkForWriting(4, BeingWrittenWord(kWriterA)));
  EXPECT_TRUE(ring->TryAcquireChunkForWriting(2, BeingWrittenWord(kWriterB)));
}

// The Free identity is a fixed 16-bit wrap count, so the same physical chunk
// answers to the same word again after num_chunks * 65536 reservations. That
// period is an accepted limit of the initial ABI, pinned here so the test
// fails if the wrap-count width silently changes.
TEST(SharedRingBufferTest, WrapIdentityRepeatsAfterTheDocumentedPeriod) {
  auto ring = SharedRingBuffer::Create(2, kChunkSize);
  ASSERT_NE(ring, nullptr);

  // A fresh chunk 0 is Free(0), tagged for position 0. One full identity
  // period later, position 2 * 65536 maps to the same chunk and its wrap
  // count - bit 16 of the shifted position - is truncated back to zero, so
  // this claim lands even though the ring never ran.
  EXPECT_TRUE(
      ring->TryAcquireChunkForWriting(2u * 65536, BeingWrittenWord(kWriterA)));
}

// The wrap count a seeded ring stamps is the 16-bit truncation of the shifted
// position, not the shifted position itself.
TEST(SharedRingBufferTest, SeededPositionsStampSixteenBitWraps) {
  auto ring = SharedRingBuffer::Create(4, kChunkSize);
  ASSERT_NE(ring, nullptr);
  // 4 * 65536 is one whole identity period, so every chunk is stamped with
  // exactly the word a fresh mapping holds.
  ring->SetPositionsForTesting(4u * 65536);
  for (uint32_t i = 0; i < 4; ++i)
    EXPECT_EQ(PeekStateWord(ring.get(), i), 0u) << i;
}

TEST(SharedRingBufferTest, PublishReuseAndReclaimAreExactValueTransitions) {
  auto ring = SharedRingBuffer::Create(4, kChunkSize);
  ASSERT_NE(ring, nullptr);

  ASSERT_EQ(ring->TryReserveWritePos().position, 0u);
  ASSERT_TRUE(ring->TryAcquireChunkForWriting(0, BeingWrittenWord(kWriterA)));

  uint32_t observed = BeingWrittenWord(kWriterA);
  ASSERT_TRUE(
      ring->TrySetChunkComplete(0, &observed, CompleteWord(kWriterA, 2)));
  EXPECT_EQ(PeekStateWord(ring.get(), 0), CompleteWord(kWriterA, 2));

  // A publication against a word that is no longer there fails and reports what
  // is.
  uint32_t stale = BeingWrittenWord(kWriterA);
  EXPECT_FALSE(ring->TrySetChunkComplete(0, &stale, CompleteWord(kWriterA, 3)));
  EXPECT_EQ(stale, CompleteWord(kWriterA, 2));

  // Reuse takes the same chunk back, keeping the published count.
  ASSERT_TRUE(ring->TryReacquireChunkForWriting(0, CompleteWord(kWriterA, 2)));
  EXPECT_EQ(ChunkStateOf(PeekStateWord(ring.get(), 0)),
            ChunkState::kBeingWritten);
  EXPECT_EQ(NumFragmentsOf(PeekStateWord(ring.get(), 0)), 2u);

  observed =
      ReplaceChunkState(CompleteWord(kWriterA, 2), ChunkState::kBeingWritten);
  ASSERT_TRUE(
      ring->TrySetChunkComplete(0, &observed, CompleteWord(kWriterA, 5)));

  // The reader consumes it and stamps the wrap for the *next* traversal of this
  // chunk, taken from the position it just resolved.
  observed = CompleteWord(kWriterA, 5);
  ASSERT_TRUE(ring->TryReleaseCompleteChunkAsFree(0, &observed));
  EXPECT_EQ(PeekStateWord(ring.get(), 0), MakeFreeStateWord(1));
}

TEST(SharedRingBufferTest, MarkForRewritePassesEveryOtherFieldThrough) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);

  const uint32_t being_written = MakeDataStateWord(
      ChunkState::kBeingWritten, ChunkFormat::kReservedRouting,
      kFlagContinuesFromPrevChunk | kFlagDataLoss, 0, kWriterA);
  ASSERT_TRUE(ring->TryAcquireChunkForWriting(0, being_written));

  const uint32_t published = MakeDataStateWord(
      ChunkState::kBeingWritten, ChunkFormat::kReservedRouting,
      kFlagContinuesFromPrevChunk | kFlagDataLoss, 3, kWriterA);
  // Simulate the writer having appended three fragments by reusing the chunk
  // through the publish/reuse pair.
  uint32_t observed = being_written;
  ASSERT_TRUE(ring->TrySetChunkComplete(
      0, &observed, ReplaceChunkState(published, ChunkState::kComplete)));
  ASSERT_TRUE(ring->TryReacquireChunkForWriting(
      0, ReplaceChunkState(published, ChunkState::kComplete)));

  observed = published;
  ASSERT_TRUE(ring->TryRequestRewrite(0, &observed));
  const uint32_t marked = PeekStateWord(ring.get(), 0);
  EXPECT_EQ(ChunkStateOf(marked), ChunkState::kRewriteRequested);
  // The reader can arbitrate a chunk whose format it does not understand
  // precisely because it changes nothing but the state.
  EXPECT_EQ(ChunkFormatOf(marked), ChunkFormat::kReservedRouting);
  EXPECT_EQ(PayloadFlagsOf(marked),
            kFlagContinuesFromPrevChunk | kFlagDataLoss);
  EXPECT_EQ(NumFragmentsOf(marked), 3u);
  EXPECT_EQ(WriterIdOf(marked), kWriterA);
}

TEST(SharedRingBufferTest, AcknowledgeThenReclaimReturnsTheChunk) {
  auto ring = SharedRingBuffer::Create(4, kChunkSize);
  ASSERT_NE(ring, nullptr);

  ASSERT_TRUE(ring->TryAcquireChunkForWriting(0, BeingWrittenWord(kWriterA)));
  uint32_t observed = BeingWrittenWord(kWriterA);
  ASSERT_TRUE(ring->TryRequestRewrite(0, &observed));
  const uint32_t marked = ReplaceChunkState(BeingWrittenWord(kWriterA),
                                            ChunkState::kRewriteRequested);
  EXPECT_EQ(PeekStateWord(ring.get(), 0), marked);

  // Nobody but the owning writer can leave RewriteRequested, and the writer
  // says nothing about who gets the chunk next.
  ASSERT_TRUE(ring->TryAcknowledgeRewrite(0, marked));
  EXPECT_EQ(PeekStateWord(ring.get(), 0), kRewriteAcknowledgedStateWord);

  // Only the reader turns that into a free word, and it stamps the wrap of the
  // position it is resolving.
  ASSERT_TRUE(ring->TryReleaseRewriteAcknowledgedChunkAsFree(4, &observed));
  EXPECT_EQ(PeekStateWord(ring.get(), 0), MakeFreeStateWord(2));
}

// The reclaim compares against the exact rewrite-acknowledgment word. A failed
// CAS reports the word that defeated it, rather than doing a second load or
// returning the caller's stale dispatch word.
TEST(SharedRingBufferTest,
     FailedRewriteAcknowledgedReclaimReportsTheDefeatingWord) {
  auto ring = SharedRingBuffer::Create(4, kChunkSize);
  ASSERT_NE(ring, nullptr);

  // RewriteAcknowledged state bits over a nonzero payload: the same dispatch
  // state, but not the one word the transition may leave from.
  const uint32_t forged = kRewriteAcknowledgedStateWord | kFlagDataLoss;
  ring->SetStateWordForTesting(0, forged);

  uint32_t observed = 0;
  EXPECT_FALSE(ring->TryReleaseRewriteAcknowledgedChunkAsFree(0, &observed));
  EXPECT_EQ(observed, forged);
  EXPECT_EQ(PeekStateWord(ring.get(), 0), forged);

  // The exact word goes through.
  ring->SetStateWordForTesting(0, kRewriteAcknowledgedStateWord);
  EXPECT_TRUE(ring->TryReleaseRewriteAcknowledgedChunkAsFree(0, &observed));
  EXPECT_EQ(PeekStateWord(ring.get(), 0), MakeFreeStateWord(1));
}

// Every word that says "this chunk is claimable" comes out of exactly three
// methods, and all three are the reader's. A writer has no way to produce one:
// the strongest thing it can say is RewriteAcknowledged.
TEST(SharedRingBufferTest, NoWriterTransitionProducesAFreeWord) {
  auto ring = SharedRingBuffer::Create(4, kChunkSize);
  ASSERT_NE(ring, nullptr);

  ASSERT_TRUE(ring->TryAcquireChunkForWriting(0, BeingWrittenWord(kWriterA)));
  uint32_t observed = BeingWrittenWord(kWriterA);
  ASSERT_TRUE(
      ring->TrySetChunkComplete(0, &observed, CompleteWord(kWriterA, 1)));
  EXPECT_NE(ChunkStateOf(PeekStateWord(ring.get(), 0)), ChunkState::kFree);

  ASSERT_TRUE(ring->TryReacquireChunkForWriting(0, CompleteWord(kWriterA, 1)));
  EXPECT_NE(ChunkStateOf(PeekStateWord(ring.get(), 0)), ChunkState::kFree);

  observed =
      ReplaceChunkState(CompleteWord(kWriterA, 1), ChunkState::kBeingWritten);
  ASSERT_TRUE(ring->TryRequestRewrite(0, &observed));
  ASSERT_TRUE(ring->TryAcknowledgeRewrite(
      0, ReplaceChunkState(CompleteWord(kWriterA, 1),
                           ChunkState::kRewriteRequested)));
  EXPECT_NE(ChunkStateOf(PeekStateWord(ring.get(), 0)), ChunkState::kFree);
  EXPECT_EQ(PeekStateWord(ring.get(), 0), kRewriteAcknowledgedStateWord);

  ASSERT_TRUE(ring->TryReleaseRewriteAcknowledgedChunkAsFree(0, &observed));
  EXPECT_EQ(ChunkStateOf(PeekStateWord(ring.get(), 0)), ChunkState::kFree);
}

TEST(SharedRingBufferTest, ReclaimStampsTheNextWrapAcrossPositionRollover) {
  // At the 32-bit position rollover the shifted position restarts from zero. A
  // reader that incremented the 16-bit value it found in the chunk would agree
  // here by coincidence of the uint16_t wrap, so the assertion that matters is
  // the exact stamped word, derived from the position.
  auto ring = SharedRingBuffer::Create(16, kChunkSize);
  ASSERT_NE(ring, nullptr);

  const uint32_t kLastLap = 0xfffffff0u;  // chunk 0, the last lap's wrap
  ASSERT_EQ(ChunkIndexOfPosition(kLastLap, 16), 0u);
  ASSERT_EQ(WrapCountForPosition(kLastLap, ring->chunk_index_bits()), 0xffffu);

  // Walk chunk 0 into RewriteAcknowledged, the state the reader reclaims.
  ASSERT_TRUE(ring->TryAcquireChunkForWriting(0, BeingWrittenWord(kWriterA)));
  uint32_t observed = BeingWrittenWord(kWriterA);
  ASSERT_TRUE(ring->TryRequestRewrite(0, &observed));
  ASSERT_TRUE(ring->TryAcknowledgeRewrite(
      0, ReplaceChunkState(BeingWrittenWord(kWriterA),
                           ChunkState::kRewriteRequested)));

  ASSERT_TRUE(
      ring->TryReleaseRewriteAcknowledgedChunkAsFree(kLastLap, &observed));
  EXPECT_EQ(PeekStateWord(ring.get(), 0), MakeFreeStateWord(0));
  // The writer holding position 0, the one that follows kLastLap on chunk 0,
  // is exactly the one that can claim it.
  EXPECT_TRUE(ring->TryAcquireChunkForWriting(0, BeingWrittenWord(kWriterA)));
}

TEST(SharedRingBufferTest, ReclaimStampsZeroAtTheWrapIdentityRollover) {
  // The 16-bit identity rolls over every num_chunks * 65536 positions, long
  // before the position does. The wrap after 0xffff is zero, and it comes from
  // the position, not from incrementing the chunk's value.
  auto ring = SharedRingBuffer::Create(4, kChunkSize);
  ASSERT_NE(ring, nullptr);

  const uint32_t kLastLap = 0xffffu * 4;  // chunk 0, wrap 0xffff
  ASSERT_EQ(ChunkIndexOfPosition(kLastLap, 4), 0u);
  ASSERT_EQ(WrapCountForPosition(kLastLap, ring->chunk_index_bits()), 0xffffu);

  ring->SetPositionsForTesting(kLastLap);
  ASSERT_EQ(PeekStateWord(ring.get(), 0), MakeFreeStateWord(0xffff));

  uint32_t observed = MakeFreeStateWord(0xffff);
  ASSERT_TRUE(ring->TryMoveFreeChunkToNextWrap(kLastLap, &observed));
  EXPECT_EQ(PeekStateWord(ring.get(), 0), MakeFreeStateWord(0));
  // The next traversal of chunk 0 belongs to position kLastLap + 4, whose wrap
  // is the truncated zero.
  EXPECT_TRUE(ring->TryAcquireChunkForWriting(kLastLap + 4,
                                              BeingWrittenWord(kWriterB)));
}

// ---------------------------------------------------------------------------
// Backpressure.
// ---------------------------------------------------------------------------

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX_BUT_NOT_QNX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)

TEST(SharedRingBufferTest, BlockedWriterWakesWhenTheReaderReleasesCapacity) {
  auto ring = SharedRingBuffer::Create(2, kChunkSize);
  ASSERT_NE(ring, nullptr);
  ASSERT_EQ(ring->TryReserveWritePos().result,
            SharedRingBuffer::ReserveResult::kReserved);
  ASSERT_EQ(ring->TryReserveWritePos().result,
            SharedRingBuffer::ReserveResult::kReserved);

  const auto full = ring->TryReserveWritePos();
  ASSERT_EQ(full.result, SharedRingBuffer::ReserveResult::kFull);

  base::WaitableEvent about_to_wait;
  std::atomic<bool> reserved{false};
  std::thread writer([&] {
    about_to_wait.Notify();
    // Bounded only as a deadlock guard: the reader below always frees capacity,
    // so a correct implementation never comes close to the timeout.
    const auto outcome =
        ring->WaitForReadPosChange(full.read_pos_sample, 30000);
    EXPECT_EQ(outcome, SharedRingBuffer::WriterWaitResult::kRetry);
    reserved.store(ring->TryReserveWritePos().result ==
                   SharedRingBuffer::ReserveResult::kReserved);
  });

  about_to_wait.Wait();
  // Wait until the writer is actually parked, so that this exercises the wake
  // rather than the early-return path.
  const bool parked =
      SpinUntil([&] { return ring->num_writers_waiting_for_testing() != 0; });

  // Published unconditionally: it is both the wake under test and what lets
  // the writer thread finish - and be joined - if parking was never observed.
  ring->PublishReadPos(1);
  writer.join();
  ASSERT_TRUE(parked);
  EXPECT_TRUE(reserved.load());
  EXPECT_EQ(ring->num_writers_waiting_for_testing(), 0u);
}

// If read_pos moves between the capacity sample and the wait, the wait must
// come straight back rather than park on a value that will never be stored
// again. Without that the writer sleeps with space available.
TEST(SharedRingBufferTest, WaitReturnsImmediatelyIfReadPosAlreadyMoved) {
  auto ring = SharedRingBuffer::Create(2, kChunkSize);
  ASSERT_NE(ring, nullptr);

  const uint32_t stale_sample = ring->read_pos_for_testing();
  ring->PublishReadPos(5);

  const auto outcome = ring->WaitForReadPosChange(stale_sample, 30000);
  EXPECT_EQ(outcome, SharedRingBuffer::WriterWaitResult::kRetry);
  EXPECT_EQ(ring->num_writers_waiting_for_testing(), 0u);
}

// Besides the timeout path, this checks the futex watches the read half of the
// packed positions: write_pos is nonzero while read_pos still equals the
// expected value, so a wait that watched the wrong half - or a misplaced
// address - would find a mismatch and return immediately instead of sleeping.
TEST(SharedRingBufferTest, WaitTimesOutWatchingTheReadHalf) {
  auto ring = SharedRingBuffer::Create(2, kChunkSize);
  ASSERT_NE(ring, nullptr);
  ASSERT_EQ(ring->TryReserveWritePos().result,
            SharedRingBuffer::ReserveResult::kReserved);
  ASSERT_EQ(ring->TryReserveWritePos().result,
            SharedRingBuffer::ReserveResult::kReserved);
  ASSERT_EQ(ring->LoadWritePos(), 2u);
  ASSERT_EQ(ring->read_pos_for_testing(), 0u);

  // 1 ms because this test relies on the timeout firing.
  EXPECT_EQ(ring->WaitForReadPosChange(0, 1),
            SharedRingBuffer::WriterWaitResult::kTimedOut);
  EXPECT_EQ(ring->num_writers_waiting_for_testing(), 0u);
}

TEST(SharedRingBufferTest, SeveralWaitersAreAllReleased) {
  constexpr uint32_t kNumWaiters = 8;
  constexpr uint32_t kWaitTimeoutMs = 1000;
  auto ring = SharedRingBuffer::Create(2, kChunkSize);
  ASSERT_NE(ring, nullptr);

  std::atomic<uint32_t> woke{0};
  std::vector<SharedRingBuffer::WriterWaitResult> outcomes(
      kNumWaiters, SharedRingBuffer::WriterWaitResult::kTimedOut);
  std::vector<std::thread> waiters;
  for (uint32_t i = 0; i < kNumWaiters; ++i) {
    waiters.emplace_back([&, i] {
      outcomes[i] = ring->WaitForReadPosChange(0, kWaitTimeoutMs);
      woke.fetch_add(1);
    });
  }

  const bool all_parked = SpinUntil(
      [&] { return ring->num_writers_waiting_for_testing() == kNumWaiters; });

  // Published - and every thread joined - before any assertion, so a failure
  // here cannot leave a joinable thread behind.
  ring->PublishReadPos(1);
  for (auto& waiter : waiters)
    waiter.join();

  ASSERT_TRUE(all_parked);
  EXPECT_EQ(woke.load(), kNumWaiters);
  for (auto outcome : outcomes)
    EXPECT_EQ(outcome, SharedRingBuffer::WriterWaitResult::kRetry);
  EXPECT_EQ(ring->num_writers_waiting_for_testing(), 0u);
}

// A publication may land anywhere between the waiter registering, the
// user-space recheck and the kernel's atomic compare: none of those windows
// may lose it. Each round parks a waiter, publishes as soon as the hint says
// it is registered - which is before it has necessarily reached the kernel -
// and requires progress, not a timeout. The generous timeout exists only so a
// lost wake fails loudly instead of hanging.
TEST(SharedRingBufferTest, PublicationBetweenRegistrationAndSleepIsNotLost) {
  constexpr uint32_t kRounds = 100;
  auto ring = SharedRingBuffer::Create(2, kChunkSize);
  ASSERT_NE(ring, nullptr);

  for (uint32_t round = 0; round < kRounds; ++round) {
    const uint32_t sample = ring->read_pos_for_testing();
    SharedRingBuffer::WriterWaitResult outcome =
        SharedRingBuffer::WriterWaitResult::kTimedOut;
    std::thread writer(
        [&] { outcome = ring->WaitForReadPosChange(sample, 30000); });

    const bool registered =
        SpinUntil([&] { return ring->num_writers_waiting_for_testing() != 0; });
    ring->PublishReadPos(sample + 1);
    writer.join();

    ASSERT_TRUE(registered) << round;
    ASSERT_EQ(outcome, SharedRingBuffer::WriterWaitResult::kRetry) << round;
    ASSERT_EQ(ring->num_writers_waiting_for_testing(), 0u) << round;
  }
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

#endif  // PERFETTO_OS_LINUX_BUT_NOT_QNX || PERFETTO_OS_ANDROID

// The errno policy of the wait, checked directly rather than by arranging for
// the kernel to fail. Every branch matters to a stalling writer: two of them
// send it back to the capacity predicate, one is the slice expiring, and the
// rest have to stop it waiting at all.
TEST(SharedRingBufferTest, WaitErrnoPolicy) {
  using WriterWaitResult = SharedRingBuffer::WriterWaitResult;
  EXPECT_EQ(SharedRingBuffer::ClassifyWriterWaitErrnoForTesting(ETIMEDOUT),
            WriterWaitResult::kTimedOut);
  // read_pos moved between the load and the syscall.
  EXPECT_EQ(SharedRingBuffer::ClassifyWriterWaitErrnoForTesting(EAGAIN),
            WriterWaitResult::kRetry);
  // A signal. Deliberately not retried inside the wrapper.
  EXPECT_EQ(SharedRingBuffer::ClassifyWriterWaitErrnoForTesting(EINTR),
            WriterWaitResult::kRetry);
  // Anything else means this address cannot be waited on, so a stalling writer
  // must drop rather than come straight back into the same failing syscall.
  EXPECT_EQ(SharedRingBuffer::ClassifyWriterWaitErrnoForTesting(EINVAL),
            WriterWaitResult::kUnavailable);
  EXPECT_EQ(SharedRingBuffer::ClassifyWriterWaitErrnoForTesting(EFAULT),
            WriterWaitResult::kUnavailable);
  EXPECT_EQ(SharedRingBuffer::ClassifyWriterWaitErrnoForTesting(ENOSYS),
            WriterWaitResult::kUnavailable);
}

}  // namespace
}  // namespace perfetto::tracing_v2
