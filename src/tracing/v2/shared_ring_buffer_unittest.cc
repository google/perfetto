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

#include <stdint.h>

#include <atomic>
#include <thread>
#include <vector>

#include "perfetto/base/time.h"
#include "perfetto/ext/base/paged_memory.h"
#include "perfetto/ext/base/waitable_event.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"
#include "src/tracing/v2/shared_ring_buffer_test_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::tracing_v2 {
namespace {

using Internals = test::SharedRingBufferInternalsForTest;
using test::WrapCountOf;

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
uint32_t PeekStateWord(SharedRingBuffer* ring, uint32_t chunk_idx) {
  const uint8_t* chunk = ring->chunk_at(chunk_idx);
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

// A page-aligned, zero-filled region for the constructor-contract tests below.
// SharedRingBufferForTesting is not used here because it never builds an
// invalid ring.
size_t RingSizeFor(uint32_t num_chunks, uint32_t chunk_size) {
  return sizeof(RingBufferHeader) +
         static_cast<size_t>(num_chunks) * chunk_size;
}

// An invalid ring layout is a configuration error, so the constructor CHECKs.
// It only does arithmetic on |size|, which is why an impossibly large region
// can be described by a small mapping.
TEST(TracingV2SharedRingBufferTest, InvalidLayout) {
  base::PagedMemory memory = base::PagedMemory::Allocate(64 * 1024);
  uint8_t* start = static_cast<uint8_t*>(memory.Get());

  // The chunk count is derived from the region: the bytes after the header
  // must divide into a non-zero power-of-two number of chunks, at most 2^30.
  EXPECT_DEATH_IF_SUPPORTED(
      { SharedRingBuffer ring(start, sizeof(RingBufferHeader), kChunkSize); },
      "PERFETTO_CHECK");
  EXPECT_DEATH_IF_SUPPORTED(
      { SharedRingBuffer ring(start, RingSizeFor(3, kChunkSize), kChunkSize); },
      "PERFETTO_CHECK");
  EXPECT_DEATH_IF_SUPPORTED(
      { SharedRingBuffer ring(start, RingSizeFor(6, kChunkSize), kChunkSize); },
      "PERFETTO_CHECK");
  EXPECT_DEATH_IF_SUPPORTED(
      {
        SharedRingBuffer ring(start, RingSizeFor(4, kChunkSize) + 1,
                              kChunkSize);
      },
      "PERFETTO_CHECK");
  if (sizeof(size_t) >= 8) {
    EXPECT_DEATH_IF_SUPPORTED(
        {
          SharedRingBuffer ring(start, RingSizeFor(1u << 31, kChunkSize),
                                kChunkSize);
        },
        "PERFETTO_CHECK");
  }

  // chunk_size must be at least 256 and keep the state word aligned.
  EXPECT_DEATH_IF_SUPPORTED(
      { SharedRingBuffer ring(start, RingSizeFor(4, 0), 0); },
      "PERFETTO_CHECK");
  EXPECT_DEATH_IF_SUPPORTED(
      { SharedRingBuffer ring(start, RingSizeFor(4, 128), 128); },
      "PERFETTO_CHECK");
  EXPECT_DEATH_IF_SUPPORTED(
      { SharedRingBuffer ring(start, RingSizeFor(4, 255), 255); },
      "PERFETTO_CHECK");
  EXPECT_DEATH_IF_SUPPORTED(
      { SharedRingBuffer ring(start, RingSizeFor(4, 258), 258); },
      "PERFETTO_CHECK");

  // On a 32-bit build, adding this chunk size to the 64-byte header wraps to
  // zero. Validation must reject the empty region before doing that addition.
  constexpr uint32_t kWrappingChunkSize = UINT32_MAX - 63u;
  EXPECT_DEATH_IF_SUPPORTED(
      { SharedRingBuffer ring(start, 0, kWrappingChunkSize); },
      "PERFETTO_CHECK");

  // The header must be present and aligned for its atomics.
  EXPECT_DEATH_IF_SUPPORTED(
      {
        SharedRingBuffer ring(start + 4, RingSizeFor(4, kChunkSize),
                              kChunkSize);
      },
      "PERFETTO_CHECK");
  EXPECT_DEATH_IF_SUPPORTED(
      {
        SharedRingBuffer ring(nullptr, RingSizeFor(4, kChunkSize), kChunkSize);
      },
      "PERFETTO_CHECK");
}

TEST(TracingV2SharedRingBufferTest, ValidLayout) {
  for (uint32_t chunk_size : {256u, 260u, 512u, 1000u, 4096u, 65536u}) {
    test::SharedRingBufferForTesting ring(4, chunk_size);
    EXPECT_EQ(ring->chunk_size(), chunk_size);
    EXPECT_EQ(ring->num_chunks(), 4u);
  }
  for (uint32_t num_chunks : {1u, 2u, 4u, 8u, 16u, 1024u}) {
    test::SharedRingBufferForTesting ring(num_chunks, kChunkSize);
    EXPECT_EQ(ring->num_chunks(), num_chunks);
  }
}

// chunk_size must keep the state word aligned and be at least 256 bytes;
// nothing requires it to be a power of two and there is no arbitrary maximum.
TEST(TracingV2SharedRingBufferTest, NonPowerOfTwoChunkSize) {
  for (uint32_t chunk_size : {260u, 1000u, 65536u}) {
    test::SharedRingBufferForTesting ring(4, chunk_size);
    EXPECT_EQ(ring->chunk_size(), chunk_size);
    EXPECT_EQ(static_cast<uint32_t>(ring->chunk_at(1) - ring->chunk_at(0)),
              chunk_size);
  }
}

// Free(0) is the all-zero word, so a fresh zero-filled mapping is
// already correct and the ring does not walk it at construction.
TEST(TracingV2SharedRingBufferTest, FreshMappingIsFree) {
  test::SharedRingBufferForTesting ring(1024, kChunkSize);
  for (uint32_t i = 0; i < ring->num_chunks(); ++i) {
    ASSERT_EQ(PeekStateWord(ring.get(), i), 0u) << i;
    ASSERT_EQ(ChunkStateOf(ring->LoadChunkStateWord(i)), ChunkState::kFree);
    ASSERT_EQ(WrapCountOf(ring->LoadChunkStateWord(i)), 0u);
  }
  EXPECT_EQ(Internals::GetReadPos(ring.get()), 0u);
  EXPECT_EQ(ring->LoadWritePos(), 0u);
  EXPECT_EQ(Internals::GetNumWritersWaiting(ring.get()), 0u);
}

TEST(TracingV2SharedRingBufferTest, StateWordAlignment) {
  for (uint32_t chunk_size : {256u, 260u, 512u, 4096u, 65536u}) {
    test::SharedRingBufferForTesting ring(8, chunk_size);
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
TEST(TracingV2SharedRingBufferTest, ChunksFollowHeader) {
  for (uint32_t chunk_size : {256u, 260u, 4096u}) {
    test::SharedRingBufferForTesting ring(4, chunk_size);
    const uintptr_t first = reinterpret_cast<uintptr_t>(ring->chunk_at(0));
    EXPECT_EQ(first % 256, 64u) << "chunk_size " << chunk_size;
    const uintptr_t last = reinterpret_cast<uintptr_t>(ring->chunk_at(3));
    EXPECT_EQ(last - first, 3u * chunk_size);
  }
}

// ---------------------------------------------------------------------------
// Reserving positions.
// ---------------------------------------------------------------------------

TEST(TracingV2SharedRingBufferTest, ReserveUntilFull) {
  // Reservations are consecutive tickets until the ring is full.
  test::SharedRingBufferForTesting ring(4, kChunkSize);

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
  EXPECT_EQ(full.read_pos_for_wait, 0u);
}

TEST(TracingV2SharedRingBufferTest, CapacityFollowsReadPos) {
  test::SharedRingBufferForTesting ring(2, kChunkSize);

  ASSERT_EQ(ring->TryReserveWritePos().result,
            SharedRingBuffer::ReserveResult::kReserved);
  ASSERT_EQ(ring->TryReserveWritePos().result,
            SharedRingBuffer::ReserveResult::kReserved);
  ASSERT_EQ(ring->TryReserveWritePos().result,
            SharedRingBuffer::ReserveResult::kFull);

  // Publishing read_pos must preserve a concurrently updated write_pos.
  ring->PublishReadPos(1);
  EXPECT_EQ(Internals::GetReadPos(ring.get()), 1u);
  EXPECT_EQ(ring->LoadWritePos(), 2u);

  const auto reservation = ring->TryReserveWritePos();
  ASSERT_EQ(reservation.result, SharedRingBuffer::ReserveResult::kReserved);
  EXPECT_EQ(reservation.position, 2u);
  EXPECT_EQ(reservation.read_pos_for_wait, 1u);
  EXPECT_EQ(Internals::GetReadPos(ring.get()), 1u);
  EXPECT_EQ(ring->LoadWritePos(), 3u);
}

// One load of the packed positions decides capacity across uint32_t rollover.
TEST(TracingV2SharedRingBufferTest, CapacityAcrossPositionRollover) {
  test::SharedRingBufferForTesting ring(8, kChunkSize);
  const uint32_t kSeed = 0xfffffffcu;  // Four positions before the rollover.
  Internals::SetPositions(ring.get(), kSeed);

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
  EXPECT_EQ(full.read_pos_for_wait, kSeed);

  ring->PublishReadPos(kSeed + 1);
  const auto reservation = ring->TryReserveWritePos();
  ASSERT_EQ(reservation.result, SharedRingBuffer::ReserveResult::kReserved);
  EXPECT_EQ(reservation.position, 4u);
  EXPECT_EQ(reservation.read_pos_for_wait, kSeed + 1);
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
TEST(TracingV2SharedRingBufferTest, ReservationLosesToPublication) {
  test::SharedRingBufferForTesting ring(4, kChunkSize);
  ASSERT_EQ(ring->TryReserveWritePos().position, 0u);
  ASSERT_EQ(ring->TryReserveWritePos().position, 1u);

  // The value the writer loaded before the reader published.
  const uint64_t stale_rw_positions = PackRwPositions(2, 0);
  ring->PublishReadPos(1);  // Now (write=2, read=1).

  const auto reservation =
      Internals::TryReserveWritePosFromSnapshot(ring.get(), stale_rw_positions);
  ASSERT_EQ(reservation.result, SharedRingBuffer::ReserveResult::kReserved);
  EXPECT_EQ(reservation.position, 2u);
  // The failed CAS returned read_pos=1. Seeing that value here proves that the
  // retry did not keep read_pos=0 from the old value.
  EXPECT_EQ(reservation.read_pos_for_wait, 1u);
  EXPECT_EQ(ring->LoadWritePos(), 3u);
  EXPECT_EQ(Internals::GetReadPos(ring.get()), 1u);
}

// The same losing CAS, but the value returned by the failure says that the
// ring is full. The retry must take the Full exit, without burning a position.
TEST(TracingV2SharedRingBufferTest, ReservationLossFindsFull) {
  test::SharedRingBufferForTesting ring(2, kChunkSize);
  ASSERT_EQ(ring->TryReserveWritePos().position, 0u);

  const uint64_t stale_rw_positions =
      PackRwPositions(1, 0);  // One outstanding: capacity left.
  ASSERT_EQ(ring->TryReserveWritePos().position, 1u);  // Now (2, 0): full.

  const auto reservation =
      Internals::TryReserveWritePosFromSnapshot(ring.get(), stale_rw_positions);
  EXPECT_EQ(reservation.result, SharedRingBuffer::ReserveResult::kFull);
  EXPECT_EQ(reservation.read_pos_for_wait, 0u);
  EXPECT_EQ(ring->LoadWritePos(), 2u);
}

// A writer reservation lands between the reader's load and its CAS. The
// publication retry must preserve the returned write_pos: had it kept the
// stale half, the reservation would be erased and write_pos would read 0.
TEST(TracingV2SharedRingBufferTest, PublicationLosesToReservation) {
  test::SharedRingBufferForTesting ring(4, kChunkSize);

  // The value loaded by the reader before the writer reserved position 0.
  const uint64_t stale_rw_positions = PackRwPositions(0, 0);
  ASSERT_EQ(ring->TryReserveWritePos().position, 0u);  // Now (write=1, read=0).

  Internals::PublishReadPosFromSnapshot(ring.get(), stale_rw_positions, 1);
  EXPECT_EQ(ring->LoadWritePos(), 1u);
  EXPECT_EQ(Internals::GetReadPos(ring.get()), 1u);
}

// The ABI accepts any power-of-two chunk count in [1, 2^30].
TEST(TracingV2SharedRingBufferTest, OneChunkRing) {
  test::SharedRingBufferForTesting ring(1, kChunkSize);
  EXPECT_EQ(ring->num_chunks(), 1u);

  const auto reservation = ring->TryReserveWritePos();
  ASSERT_EQ(reservation.result, SharedRingBuffer::ReserveResult::kReserved);
  EXPECT_EQ(reservation.position, 0u);
  EXPECT_EQ(ring->TryReserveWritePos().result,
            SharedRingBuffer::ReserveResult::kFull);

  ASSERT_TRUE(ring->TryAcquireChunkForWriting(0, BeingWrittenWord(kWriterA)));
  uint32_t observed = BeingWrittenWord(kWriterA);
  ASSERT_TRUE(
      ring->TryReleaseChunkAsComplete(0, &observed, CompleteWord(kWriterA, 1)));
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

TEST(TracingV2SharedRingBufferTest, Claim) {
  // A claim takes the chunk of the reserved position.
  test::SharedRingBufferForTesting ring(4, kChunkSize);

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
TEST(TracingV2SharedRingBufferTest, StaleClaimFails) {
  test::SharedRingBufferForTesting ring(4, kChunkSize);

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
    const uint32_t chunk_idx = ChunkIndexOfPosition(position, 4);
    uint32_t observed = ring->LoadChunkStateWord(chunk_idx);
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

TEST(TracingV2SharedRingBufferTest, OnlyMatchingPositionClaims) {
  // Two stale claimants cannot both adopt the word a failed claim returns.
  test::SharedRingBufferForTesting ring(2, kChunkSize);

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
TEST(TracingV2SharedRingBufferTest, WrapIdentityPeriod) {
  test::SharedRingBufferForTesting ring(2, kChunkSize);

  // A fresh chunk 0 is Free(0), tagged for position 0. One full identity
  // period later, position 2 * 65536 maps to the same chunk and its wrap
  // count - bit 16 of the traversal number - is truncated back to zero, so
  // this claim lands even though the ring never ran.
  EXPECT_TRUE(
      ring->TryAcquireChunkForWriting(2u * 65536, BeingWrittenWord(kWriterA)));
}

// The wrap count a seeded ring stamps is the low 16 bits of the traversal
// number.
TEST(TracingV2SharedRingBufferTest, SeededWrapCounts) {
  test::SharedRingBufferForTesting ring(4, kChunkSize);
  // 4 * 65536 is one whole identity period, so every chunk is stamped with
  // exactly the word a fresh mapping holds.
  Internals::SetPositions(ring.get(), 4u * 65536);
  for (uint32_t i = 0; i < 4; ++i)
    EXPECT_EQ(PeekStateWord(ring.get(), i), 0u) << i;
}

TEST(TracingV2SharedRingBufferTest, PublishReuseReclaim) {
  // Publish, reuse and reclaim are exact-value transitions.
  test::SharedRingBufferForTesting ring(4, kChunkSize);

  ASSERT_EQ(ring->TryReserveWritePos().position, 0u);
  ASSERT_TRUE(ring->TryAcquireChunkForWriting(0, BeingWrittenWord(kWriterA)));

  uint32_t observed = BeingWrittenWord(kWriterA);
  ASSERT_TRUE(
      ring->TryReleaseChunkAsComplete(0, &observed, CompleteWord(kWriterA, 2)));
  EXPECT_EQ(PeekStateWord(ring.get(), 0), CompleteWord(kWriterA, 2));

  // A publication against a word that is no longer there fails and reports what
  // is.
  uint32_t stale = BeingWrittenWord(kWriterA);
  EXPECT_FALSE(
      ring->TryReleaseChunkAsComplete(0, &stale, CompleteWord(kWriterA, 3)));
  EXPECT_EQ(stale, CompleteWord(kWriterA, 2));

  // Reuse takes the same chunk back, keeping the published count.
  ASSERT_TRUE(ring->TryReacquireChunkForWriting(0, CompleteWord(kWriterA, 2)));
  EXPECT_EQ(ChunkStateOf(PeekStateWord(ring.get(), 0)),
            ChunkState::kBeingWritten);
  EXPECT_EQ(NumFragmentsOf(PeekStateWord(ring.get(), 0)), 2u);

  observed =
      ReplaceChunkState(CompleteWord(kWriterA, 2), ChunkState::kBeingWritten);
  ASSERT_TRUE(
      ring->TryReleaseChunkAsComplete(0, &observed, CompleteWord(kWriterA, 5)));

  // The reader consumes it and stamps the wrap for the *next* traversal of this
  // chunk, taken from the position it just resolved.
  observed = CompleteWord(kWriterA, 5);
  ASSERT_TRUE(ring->TryReleaseCompleteChunkAsFree(0, &observed));
  EXPECT_EQ(PeekStateWord(ring.get(), 0), MakeFreeStateWord(1));
}

TEST(TracingV2SharedRingBufferTest, RequestRewriteKeepsFields) {
  // A rewrite request passes every field but the state through.
  test::SharedRingBufferForTesting ring(4, 512);

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
  ASSERT_TRUE(ring->TryReleaseChunkAsComplete(
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
  EXPECT_EQ(WriterIDOf(marked), kWriterA);
}

TEST(TracingV2SharedRingBufferTest, AcknowledgeThenReclaim) {
  // Acknowledging and then reclaiming returns the chunk to Free.
  test::SharedRingBufferForTesting ring(4, kChunkSize);

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
TEST(TracingV2SharedRingBufferTest, FailedReclaimReportsWord) {
  test::SharedRingBufferForTesting ring(4, kChunkSize);

  // RewriteAcknowledged state bits over a nonzero payload: the same dispatch
  // state, but not the one word the transition may leave from.
  const uint32_t forged = kRewriteAcknowledgedStateWord | kFlagDataLoss;
  Internals::SetChunkStateWord(ring.get(), 0, forged);

  uint32_t observed = 0;
  EXPECT_FALSE(ring->TryReleaseRewriteAcknowledgedChunkAsFree(0, &observed));
  EXPECT_EQ(observed, forged);
  EXPECT_EQ(PeekStateWord(ring.get(), 0), forged);

  // The exact word goes through.
  Internals::SetChunkStateWord(ring.get(), 0, kRewriteAcknowledgedStateWord);
  EXPECT_TRUE(ring->TryReleaseRewriteAcknowledgedChunkAsFree(0, &observed));
  EXPECT_EQ(PeekStateWord(ring.get(), 0), MakeFreeStateWord(1));
}

// Every word that says "this chunk is claimable" comes out of exactly three
// methods, and all three are the reader's. A writer has no way to produce one:
// the strongest thing it can say is RewriteAcknowledged.
TEST(TracingV2SharedRingBufferTest, OnlyReaderWritesFree) {
  test::SharedRingBufferForTesting ring(4, kChunkSize);

  ASSERT_TRUE(ring->TryAcquireChunkForWriting(0, BeingWrittenWord(kWriterA)));
  uint32_t observed = BeingWrittenWord(kWriterA);
  ASSERT_TRUE(
      ring->TryReleaseChunkAsComplete(0, &observed, CompleteWord(kWriterA, 1)));
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

TEST(TracingV2SharedRingBufferTest, ReclaimAcrossPositionRollover) {
  // At the 32-bit position rollover the traversal number restarts from zero. A
  // reader that incremented the 16-bit value it found in the chunk would agree
  // here by coincidence of the uint16_t wrap, so the assertion that matters is
  // the exact stamped word, derived from the position.
  test::SharedRingBufferForTesting ring(16, kChunkSize);

  const uint32_t kLastLap = 0xfffffff0u;  // chunk 0, the last lap's wrap
  ASSERT_EQ(ChunkIndexOfPosition(kLastLap, 16), 0u);
  ASSERT_EQ(WrapCountForPosition(kLastLap, ring->num_chunks()), 0xffffu);

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

TEST(TracingV2SharedRingBufferTest, ReclaimAcrossWrapIdentityRollover) {
  // The 16-bit identity rolls over every num_chunks * 65536 positions, long
  // before the position does. The wrap after 0xffff is zero, and it comes from
  // the position, not from incrementing the chunk's value.
  test::SharedRingBufferForTesting ring(4, kChunkSize);

  const uint32_t kLastLap = 0xffffu * 4;  // chunk 0, wrap 0xffff
  ASSERT_EQ(ChunkIndexOfPosition(kLastLap, 4), 0u);
  ASSERT_EQ(WrapCountForPosition(kLastLap, ring->num_chunks()), 0xffffu);

  Internals::SetPositions(ring.get(), kLastLap);
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

TEST(TracingV2SharedRingBufferTest, WriterWakesOnPublish) {
  // A blocked writer wakes when the reader publishes capacity.
  if (!SharedRingBuffer::SupportsWriterWait())
    GTEST_SKIP() << "The futex wait is not available on this platform";
  test::SharedRingBufferForTesting ring(2, kChunkSize);
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
        ring->WaitForReadPosChange(full.read_pos_for_wait, 30000);
    EXPECT_EQ(outcome, SharedRingBuffer::WriterWaitResult::kRetry);
    reserved.store(ring->TryReserveWritePos().result ==
                   SharedRingBuffer::ReserveResult::kReserved);
  });

  about_to_wait.Wait();
  // Wait until the writer is actually parked, so that this exercises the wake
  // rather than the early-return path.
  const bool parked = SpinUntil(
      [&] { return Internals::GetNumWritersWaiting(ring.get()) != 0; });

  // Published unconditionally: it is both the wake under test and what lets
  // the writer thread finish - and be joined - if parking was never observed.
  ring->PublishReadPos(1);
  writer.join();
  ASSERT_TRUE(parked);
  EXPECT_TRUE(reserved.load());
  EXPECT_EQ(Internals::GetNumWritersWaiting(ring.get()), 0u);
}

// If read_pos moves between the capacity sample and the wait, the wait must
// come straight back rather than park on a value that will never be stored
// again. Without that the writer sleeps with space available.
TEST(TracingV2SharedRingBufferTest, WaitReturnsIfReadPosMoved) {
  if (!SharedRingBuffer::SupportsWriterWait())
    GTEST_SKIP() << "The futex wait is not available on this platform";
  test::SharedRingBufferForTesting ring(2, kChunkSize);

  const uint32_t stale_sample = Internals::GetReadPos(ring.get());
  ring->PublishReadPos(5);

  const auto outcome = ring->WaitForReadPosChange(stale_sample, 30000);
  EXPECT_EQ(outcome, SharedRingBuffer::WriterWaitResult::kRetry);
  EXPECT_EQ(Internals::GetNumWritersWaiting(ring.get()), 0u);
}

// Besides the timeout path, this checks the futex watches the read half of the
// packed positions: write_pos is nonzero while read_pos still equals the
// expected value, so a wait that watched the wrong half - or a misplaced
// address - would find a mismatch and return immediately instead of sleeping.
TEST(TracingV2SharedRingBufferTest, WaitTimesOut) {
  if (!SharedRingBuffer::SupportsWriterWait())
    GTEST_SKIP() << "The futex wait is not available on this platform";
  test::SharedRingBufferForTesting ring(2, kChunkSize);
  ASSERT_EQ(ring->TryReserveWritePos().result,
            SharedRingBuffer::ReserveResult::kReserved);
  ASSERT_EQ(ring->TryReserveWritePos().result,
            SharedRingBuffer::ReserveResult::kReserved);
  ASSERT_EQ(ring->LoadWritePos(), 2u);
  ASSERT_EQ(Internals::GetReadPos(ring.get()), 0u);

  // 1 ms because this test relies on the timeout firing. The lower bound on
  // the elapsed time is what proves the call slept instead of returning at
  // once; the kernel never wakes a timed wait early, so it cannot flake.
  const base::TimeNanos start = base::GetWallTimeNs();
  EXPECT_EQ(ring->WaitForReadPosChange(0, 1),
            SharedRingBuffer::WriterWaitResult::kRetry);
  EXPECT_GE(base::GetWallTimeNs() - start, base::TimeMillis(1));
  EXPECT_EQ(Internals::GetNumWritersWaiting(ring.get()), 0u);
}

TEST(TracingV2SharedRingBufferTest, AllWaitersReleased) {
  // One publication releases every parked writer.
  if (!SharedRingBuffer::SupportsWriterWait())
    GTEST_SKIP() << "The futex wait is not available on this platform";
  constexpr uint32_t kNumWaiters = 8;
  // Never fires: the publication below releases every waiter.
  constexpr uint32_t kWaitTimeoutMs = 30000;
  test::SharedRingBufferForTesting ring(2, kChunkSize);

  std::atomic<uint32_t> woke{0};
  std::vector<SharedRingBuffer::WriterWaitResult> outcomes(
      kNumWaiters, SharedRingBuffer::WriterWaitResult::kUnavailable);
  std::vector<std::thread> waiters;
  for (uint32_t i = 0; i < kNumWaiters; ++i) {
    waiters.emplace_back([&, i] {
      outcomes[i] = ring->WaitForReadPosChange(0, kWaitTimeoutMs);
      woke.fetch_add(1);
    });
  }

  const bool all_parked = SpinUntil([&] {
    return Internals::GetNumWritersWaiting(ring.get()) == kNumWaiters;
  });

  // Published - and every thread joined - before any assertion, so a failure
  // here cannot leave a joinable thread behind.
  ring->PublishReadPos(1);
  for (auto& waiter : waiters)
    waiter.join();

  ASSERT_TRUE(all_parked);
  EXPECT_EQ(woke.load(), kNumWaiters);
  for (auto outcome : outcomes)
    EXPECT_EQ(outcome, SharedRingBuffer::WriterWaitResult::kRetry);
  EXPECT_EQ(Internals::GetNumWritersWaiting(ring.get()), 0u);
}

// A publication may land anywhere between the waiter registering, the
// user-space recheck and the kernel's atomic compare: none of those windows
// may lose it. Each round parks a waiter, publishes as soon as the hint says
// it is registered - which is before it has necessarily reached the kernel -
// and requires progress, not a timeout. The generous timeout exists only so a
// lost wake fails loudly instead of hanging.
TEST(TracingV2SharedRingBufferTest, NoLostWake) {
  if (!SharedRingBuffer::SupportsWriterWait())
    GTEST_SKIP() << "The futex wait is not available on this platform";
  constexpr uint32_t kRounds = 100;
  test::SharedRingBufferForTesting ring(2, kChunkSize);

  for (uint32_t round = 0; round < kRounds; ++round) {
    const uint32_t sample = Internals::GetReadPos(ring.get());
    SharedRingBuffer::WriterWaitResult outcome =
        SharedRingBuffer::WriterWaitResult::kUnavailable;
    std::thread writer(
        [&] { outcome = ring->WaitForReadPosChange(sample, 30000); });

    const bool registered = SpinUntil(
        [&] { return Internals::GetNumWritersWaiting(ring.get()) != 0; });
    ring->PublishReadPos(sample + 1);
    writer.join();

    ASSERT_TRUE(registered) << round;
    ASSERT_EQ(outcome, SharedRingBuffer::WriterWaitResult::kRetry) << round;
    ASSERT_EQ(Internals::GetNumWritersWaiting(ring.get()), 0u) << round;
  }
}

// The hint exists only to elide a syscall. A reader that never looks at it is
// slower, not wrong, so a writer must make progress even then.
TEST(TracingV2SharedRingBufferTest, WaiterHintOptional) {
  if (!SharedRingBuffer::SupportsWriterWait())
    GTEST_SKIP() << "The futex wait is not available on this platform";
  test::SharedRingBufferForTesting ring(2, kChunkSize);
  EXPECT_EQ(Internals::GetNumWritersWaiting(ring.get()), 0u);

  // Publishing with no waiters must not wake anything or leave the hint dirty.
  ring->PublishReadPos(3);
  EXPECT_EQ(Internals::GetNumWritersWaiting(ring.get()), 0u);
  EXPECT_EQ(Internals::GetReadPos(ring.get()), 3u);
}

}  // namespace
}  // namespace perfetto::tracing_v2
