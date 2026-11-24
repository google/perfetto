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

#include "src/tracing/v2/shared_ring_buffer_writer.h"

#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

#include "perfetto/base/time.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"
#include "src/tracing/v2/shared_ring_buffer_test_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::tracing_v2 {
namespace {

using Internals = test::SharedRingBufferInternalsForTest;
using BeginFragmentResult = SharedRingBufferWriter::BeginFragmentResult;
using EndFragmentResult = SharedRingBufferWriter::EndFragmentResult;
using test::MakeWriter;
using test::PinAllChunks;
using test::WriteFragment;

constexpr WriterID kWriterA = 7;
constexpr WriterID kWriterB = 8;
constexpr BufferID kBuffer = 0x1234;

// A minimal, independent decoder for what a chunk holds. It deliberately does
// not go through SharedRingBufferReader, so that a writer test cannot pass
// because the reader shares the same misunderstanding.
struct DecodedChunk {
  ChunkState state = ChunkState::kFree;
  WriterID writer_id = 0;
  BufferID target_buffer = 0;
  uint32_t payload_flags = 0;
  std::vector<std::string> fragments;
};

DecodedChunk Decode(SharedRingBuffer* ring, ChunkIndex chunk_idx) {
  const uint8_t* chunk = ring->chunk_at(chunk_idx);
  const uint32_t word = ring->LoadChunkStateWordAcquire(chunk_idx);
  DecodedChunk decoded;
  decoded.state = ChunkStateOf(word);
  if (!HasDataFields(decoded.state))
    return decoded;

  decoded.writer_id = WriterIDOf(word);
  decoded.target_buffer = LoadTargetBufferId(chunk);
  decoded.payload_flags = PayloadFlagsOf(word);

  const uint8_t* sizes_cursor = chunk + ring->chunk_size();
  uint32_t offset = kTargetBufferPayloadOffset;
  for (uint32_t i = 0; i < NumFragmentsOf(word); ++i) {
    const auto size = ReadFragmentSizeReversed(
        chunk + kTargetBufferPayloadOffset, &sizes_cursor);
    EXPECT_TRUE(size.has_value());
    if (!size)
      return decoded;
    decoded.fragments.emplace_back(
        reinterpret_cast<const char*>(chunk + offset), *size);
    offset += *size;
  }
  return decoded;
}

// Plays the reader's part of the scrape: marks whatever the writer currently
// holds as rewrite-requested and reports the published fragment count.
uint32_t MarkForRewrite(SharedRingBuffer* ring, ChunkIndex chunk_idx) {
  uint32_t observed = ring->LoadChunkStateWordAcquire(chunk_idx);
  EXPECT_EQ(ChunkStateOf(observed), ChunkState::kBeingWritten);
  const uint32_t num_fragments_read = NumFragmentsOf(observed);
  EXPECT_TRUE(ring->TryRequestRewrite(chunk_idx, &observed));
  return num_fragments_read;
}

// Plays the reader for the oldest position. The chunk at read_pos must be
// Complete or RewriteAcknowledged. It becomes Free, and read_pos moves past
// it.
void ReleaseChunkAtReadPos(SharedRingBuffer* ring) {
  const uint32_t read_pos = Internals::GetReadPos(ring);
  const ChunkIndex chunk_idx =
      ChunkIndex::FromPosition(read_pos, ring->num_chunks());
  uint32_t observed = ring->LoadChunkStateWordAcquire(chunk_idx);
  if (ChunkStateOf(observed) == ChunkState::kRewriteAcknowledged) {
    ASSERT_TRUE(
        ring->TryReleaseRewriteAcknowledgedChunkAsFree(read_pos, &observed));
  } else {
    ASSERT_EQ(ChunkStateOf(observed), ChunkState::kComplete);
    ASSERT_TRUE(ring->TryReleaseCompleteChunkAsFree(read_pos, &observed));
  }
  ring->PublishReadPos(read_pos + 1);
}

// Publishes one fragment in each chunk. The ring buffer then stays full until
// the reader releases a chunk.
void FillRingBuffer(SharedRingBuffer* ring) {
  SharedRingBufferWriter filler(ring, kWriterA, kBuffer);
  for (uint32_t i = 0; i < ring->num_chunks(); ++i) {
    ASSERT_TRUE(WriteFragment(&filler, "filler"));
    filler.FinishCurrentChunk();
  }
}

// ---------------------------------------------------------------------------
// Fragments and their sizes.
// ---------------------------------------------------------------------------

TEST(SharedRingBufferWriterTest, PayloadUpSizesDown) {
  // Payload grows up from the header. Sizes grow down from the end.
  test::SharedRingBufferForTesting ring(4, 256);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);

  ASSERT_TRUE(WriteFragment(&writer, std::string(5, 'a')));
  ASSERT_TRUE(WriteFragment(&writer, std::string(200, 'b')));
  ASSERT_TRUE(WriteFragment(&writer, std::string(3, 'c')));

  // A 256-byte target-buffer chunk, byte for byte.
  const uint8_t* chunk = ring->chunk_at(ChunkIndex::FromIndex(0));
  EXPECT_EQ(chunk[255], 5u);
  EXPECT_EQ(chunk[254], 0xc8u);
  EXPECT_EQ(chunk[253], 1u);
  EXPECT_EQ(chunk[252], 3u);

  const DecodedChunk decoded = Decode(ring.get(), ChunkIndex::FromIndex(0));
  EXPECT_EQ(decoded.state, ChunkState::kComplete);
  EXPECT_EQ(decoded.writer_id, kWriterA);
  EXPECT_EQ(decoded.target_buffer, kBuffer);
  ASSERT_EQ(decoded.fragments.size(), 3u);
  EXPECT_EQ(decoded.fragments[0], std::string(5, 'a'));
  EXPECT_EQ(decoded.fragments[1], std::string(200, 'b'));
  EXPECT_EQ(decoded.fragments[2], std::string(3, 'c'));
}

TEST(SharedRingBufferWriterTest, FragmentSizeBoundaries) {
  // These sizes straddle the one- and two-byte varint boundary.
  test::SharedRingBufferForTesting ring(8, 512);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);

  const uint32_t kSizes[] = {0, 1, 127, 128, 255, 256};
  std::vector<std::string> expected;
  for (uint32_t size : kSizes) {
    const std::string bytes(size, static_cast<char>('A' + (size % 26)));
    ASSERT_TRUE(WriteFragment(&writer, bytes)) << size;
    expected.push_back(bytes);
  }

  // 0 + 1 + 127 + 128 + 255 + 256 = 767 bytes, which does not fit in one
  // 512-byte chunk, so the writer moved on part way through. Walk every chunk
  // it used and check the fragments come out in order.
  std::vector<std::string> seen;
  for (uint32_t chunk_idx = 0; chunk_idx < ring->num_chunks(); ++chunk_idx) {
    for (const std::string& fragment :
         Decode(ring.get(), ChunkIndex::FromIndex(chunk_idx)).fragments)
      seen.push_back(fragment);
  }
  EXPECT_EQ(seen, expected);
}

TEST(SharedRingBufferWriterTest, LargestFragment) {
  // The largest fragment fits exactly. One more byte is too large.
  test::SharedRingBufferForTesting ring(4, 256);
  // 256 - 6 header bytes - varint(248), which takes two bytes.
  constexpr uint32_t kLargest = 248;

  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);
  const auto range = writer.BeginFragment(kLargest, false);
  ASSERT_EQ(range.result, BeginFragmentResult::kSuccess);
  EXPECT_EQ(static_cast<uint32_t>(range.end - range.begin), kLargest);
  memset(range.begin, 'z', kLargest);
  ASSERT_EQ(writer.EndFragment(kLargest, false), EndFragmentResult::kSuccess);

  ASSERT_EQ(Decode(ring.get(), ChunkIndex::FromIndex(0)).fragments.size(), 1u);
  EXPECT_EQ(Decode(ring.get(), ChunkIndex::FromIndex(0)).fragments[0].size(),
            kLargest);

  // One byte more than any chunk of this size could ever hold is a caller
  // bug, not backpressure, and says so.
  SharedRingBufferWriter other = MakeWriter(ring.get(), kWriterB, kBuffer);
  EXPECT_EQ(other.BeginFragment(kLargest + 1, false).result,
            BeginFragmentResult::kTooLarge);
}

TEST(SharedRingBufferWriterTest, MaxFragmentsPerChunk) {
  // A chunk closes at 255 fragments even with payload space left.
  test::SharedRingBufferForTesting ring(4, 32768);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);

  for (uint32_t i = 0; i < kMaxFragmentsPerChunk; ++i)
    ASSERT_TRUE(WriteFragment(&writer, "")) << i;

  const DecodedChunk first = Decode(ring.get(), ChunkIndex::FromIndex(0));
  EXPECT_EQ(first.fragments.size(), kMaxFragmentsPerChunk);
  ASSERT_TRUE(WriteFragment(&writer, "x"));
  const DecodedChunk second = Decode(ring.get(), ChunkIndex::FromIndex(1));
  ASSERT_EQ(second.fragments.size(), 1u);
  EXPECT_EQ(second.fragments[0], "x");
}

TEST(SharedRingBufferWriterTest, LargeFragment) {
  // The largest chunk holds one fragment with a three-byte size varint:
  // 6 header bytes + 65527 payload bytes + 3 varint bytes = 65536.
  constexpr uint32_t kBigChunk = kMaxChunkSize;
  constexpr uint32_t kLargest = kBigChunk - 9;
  test::SharedRingBufferForTesting ring(2, kBigChunk);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);

  EXPECT_EQ(writer.BeginFragment(kLargest + 1, false).result,
            BeginFragmentResult::kTooLarge);

  const auto range = writer.BeginFragment(1, false);
  ASSERT_EQ(range.result, BeginFragmentResult::kSuccess);
  EXPECT_EQ(static_cast<uint32_t>(range.end - range.begin), kLargest);
  memset(range.begin, 'a', kLargest);
  ASSERT_EQ(writer.EndFragment(kLargest, false), EndFragmentResult::kSuccess);

  const DecodedChunk decoded = Decode(ring.get(), ChunkIndex::FromIndex(0));
  ASSERT_EQ(decoded.fragments.size(), 1u);
  EXPECT_EQ(decoded.fragments[0], std::string(kLargest, 'a'));
}

// After publishing, the writer keeps its chunk only while another fragment can
// still fit. BeginFragment() hands out a non-empty writable range even for a
// zero-length fragment, and the size varint needs one more byte, so a two-byte
// gap keeps the chunk and a gap of one byte or none lets it go. The probe is
// an empty fragment: anything larger would be turned away by BeginFragment()
// itself, which would hide the retention decision.
TEST(SharedRingBufferWriterTest, ResidualSpaceBoundary) {
  for (uint32_t residual : {0u, 1u, 2u}) {
    SCOPED_TRACE(residual);
    test::SharedRingBufferForTesting ring(4, 256);
    SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);

    // 256 - 6 header bytes leaves 250. A fragment of 128 bytes or more takes
    // a two-byte size, so 248 - size bytes remain after it.
    const uint32_t first_size = 248 - residual;
    ASSERT_TRUE(WriteFragment(&writer, std::string(first_size, 'a')));
    ASSERT_TRUE(WriteFragment(&writer, ""));

    const bool kept_chunk = residual == 2;
    EXPECT_EQ(ring->LoadWritePosRelaxed(), kept_chunk ? 1u : 2u);
    const DecodedChunk first = Decode(ring.get(), ChunkIndex::FromIndex(0));
    ASSERT_EQ(first.fragments.size(), kept_chunk ? 2u : 1u);
    EXPECT_EQ(first.fragments[0].size(), first_size);
    if (kept_chunk) {
      EXPECT_TRUE(first.fragments[1].empty());
    } else {
      EXPECT_EQ(Decode(ring.get(), ChunkIndex::FromIndex(1)).fragments,
                std::vector<std::string>{""});
    }
  }
}

// An aligned, non-power-of-two chunk size, end to end through the writer.
TEST(SharedRingBufferWriterTest, NonPowerOfTwoChunkSize) {
  test::SharedRingBufferForTesting ring(4, 260);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);

  // 260 - 6 header bytes - the two-byte varint for 252.
  constexpr uint32_t kLargest = 252;
  EXPECT_EQ(writer.BeginFragment(kLargest + 1, false).result,
            BeginFragmentResult::kTooLarge);
  ASSERT_TRUE(WriteFragment(&writer, std::string(kLargest, 'z')));

  ASSERT_TRUE(WriteFragment(&writer, "next"));
  ASSERT_EQ(Decode(ring.get(), ChunkIndex::FromIndex(0)).fragments.size(), 1u);
  EXPECT_EQ(Decode(ring.get(), ChunkIndex::FromIndex(0)).fragments[0].size(),
            kLargest);
  const DecodedChunk second = Decode(ring.get(), ChunkIndex::FromIndex(1));
  ASSERT_EQ(second.fragments.size(), 1u);
  EXPECT_EQ(second.fragments[0], "next");
}

// ---------------------------------------------------------------------------
// Chunk reuse and the payload flags.
// ---------------------------------------------------------------------------

TEST(SharedRingBufferWriterTest, ReusesCompleteChunk) {
  // The writer reuses its own Complete chunk while it has room.
  test::SharedRingBufferForTesting ring(4, 512);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);

  ASSERT_TRUE(WriteFragment(&writer, "one"));
  ASSERT_TRUE(WriteFragment(&writer, "two"));

  // Both fragments are in the same physical chunk, and only one position was
  // consumed.
  EXPECT_EQ(ring->LoadWritePosRelaxed(), 1u);
  const DecodedChunk decoded = Decode(ring.get(), ChunkIndex::FromIndex(0));
  ASSERT_EQ(decoded.fragments.size(), 2u);
  EXPECT_EQ(decoded.fragments[0], "one");
  EXPECT_EQ(decoded.fragments[1], "two");
  EXPECT_EQ(
      ChunkStateOf(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(1))),
      ChunkState::kFree);
}

TEST(SharedRingBufferWriterTest, ReuseLosesToReclaim) {
  // Reuse loses to the reader reclaiming the chunk first.
  test::SharedRingBufferForTesting ring(4, 512);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);

  ASSERT_TRUE(WriteFragment(&writer, "one"));

  // The reader consumes the Complete chunk before the writer takes it back.
  uint32_t observed = ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0));
  ASSERT_EQ(ChunkStateOf(observed), ChunkState::kComplete);
  ASSERT_TRUE(ring->TryReleaseCompleteChunkAsFree(0, &observed));
  ring->PublishReadPos(1);

  // The writer's reuse fails. It drops its handle and goes for a fresh chunk
  // rather than writing behind the reader.
  ASSERT_TRUE(WriteFragment(&writer, "two"));
  EXPECT_EQ(
      ChunkStateOf(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0))),
      ChunkState::kFree);
  const DecodedChunk decoded = Decode(ring.get(), ChunkIndex::FromIndex(1));
  ASSERT_EQ(decoded.fragments.size(), 1u);
  EXPECT_EQ(decoded.fragments[0], "two");
}

// A chunk published with "continues on next chunk" is never reused. Without
// that rule a later scrape could take a set of fragments ending in the middle
// of a packet.
TEST(SharedRingBufferWriterTest, ContinuesOnNextNotReused) {
  test::SharedRingBufferForTesting ring(4, 512);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);

  ASSERT_TRUE(WriteFragment(&writer, "head", /*continues_from_prev=*/false,
                            /*continues_on_next=*/true));
  ASSERT_TRUE(WriteFragment(&writer, "tail", /*continues_from_prev=*/true,
                            /*continues_on_next=*/false));

  const DecodedChunk first = Decode(ring.get(), ChunkIndex::FromIndex(0));
  EXPECT_EQ(first.payload_flags, kFlagContinuesOnNextChunk);
  ASSERT_EQ(first.fragments.size(), 1u);
  EXPECT_EQ(first.fragments[0], "head");

  const DecodedChunk second = Decode(ring.get(), ChunkIndex::FromIndex(1));
  EXPECT_EQ(second.payload_flags, kFlagContinuesFromPrevChunk);
  ASSERT_EQ(second.fragments.size(), 1u);
  EXPECT_EQ(second.fragments[0], "tail");
}

TEST(SharedRingBufferWriterTest, DataLossOnNextChunk) {
  // A loss is reported on the next chunk, and only once.
  test::SharedRingBufferForTesting ring(4, 512);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);

  writer.RecordDataLoss();
  ASSERT_TRUE(WriteFragment(&writer, "after the gap"));
  EXPECT_EQ(Decode(ring.get(), ChunkIndex::FromIndex(0)).payload_flags,
            kFlagDataLoss);

  // The chunk after it describes no gap of its own.
  ASSERT_TRUE(WriteFragment(&writer, std::string(500, 'x')));
  EXPECT_EQ(Decode(ring.get(), ChunkIndex::FromIndex(1)).payload_flags, 0u);
}

// Pending loss must not force a new reservation while cached space is usable.
TEST(SharedRingBufferWriterTest, DataLossReusesCachedChunkWhenFull) {
  test::SharedRingBufferForTesting ring(2, 512);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);
  SharedRingBufferWriter blocker = MakeWriter(ring.get(), kWriterB, kBuffer);
  ASSERT_TRUE(WriteFragment(&writer, "before"));
  ASSERT_TRUE(WriteFragment(&blocker, "occupied"));
  ASSERT_EQ(ring->LoadWritePosRelaxed(), 2u);

  writer.RecordDataLoss();
  ASSERT_TRUE(WriteFragment(&writer, "after"));
  ASSERT_TRUE(WriteFragment(&writer, "more"));

  // All three fragments stay in the same chunk. Its flag tells the reader
  // to discard them together, without locating the gap between them.
  EXPECT_EQ(ring->LoadWritePosRelaxed(), 2u);
  const DecodedChunk chunk = Decode(ring.get(), ChunkIndex::FromIndex(0));
  EXPECT_EQ(chunk.fragments,
            (std::vector<std::string>{"before", "after", "more"}));
  EXPECT_EQ(chunk.payload_flags, kFlagDataLoss);
  EXPECT_EQ(writer.GetStats().failed_claims, 0u);
}

TEST(SharedRingBufferWriterTest, PendingLossSurvivesRepeatedFullBuffer) {
  test::SharedRingBufferForTesting ring(2, 512);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);
  SharedRingBufferWriter blocker = MakeWriter(ring.get(), kWriterB, kBuffer);
  ASSERT_TRUE(WriteFragment(&writer, "before"));
  ASSERT_TRUE(WriteFragment(&blocker, "occupied"));
  ASSERT_EQ(blocker.FinishCurrentChunk(), EndFragmentResult::kSuccess);

  ASSERT_EQ(writer.FinishCurrentChunk(), EndFragmentResult::kSuccess);
  const ChunkIndex first_idx = ChunkIndex::FromIndex(0);
  const uint32_t before_word = ring->LoadChunkStateWordAcquire(first_idx);
  writer.RecordDataLoss();

  // Without a cached chunk, every attempt needs a reservation.
  // A full ring buffer must leave the positions and old data unchanged.
  for (uint32_t attempt = 0; attempt < 3; ++attempt) {
    SCOPED_TRACE(attempt);
    EXPECT_EQ(writer.BeginFragment(1, false).result,
              BeginFragmentResult::kFull);
    EXPECT_EQ(ring->LoadWritePosRelaxed(), 2u);
    EXPECT_EQ(ring->LoadChunkStateWordAcquire(first_idx), before_word);
    const DecodedChunk before = Decode(ring.get(), first_idx);
    EXPECT_EQ(before.fragments, std::vector<std::string>{"before"});
    EXPECT_EQ(before.payload_flags, 0u);
  }
  EXPECT_EQ(writer.GetStats().failed_claims, 0u);

  // Once the reader frees space, the next chunk still carries the pending gap.
  uint32_t expected = before_word;
  ASSERT_TRUE(ring->TryReleaseCompleteChunkAsFree(0, &expected));
  ring->PublishReadPos(1);
  ASSERT_TRUE(WriteFragment(&writer, "after"));
  ASSERT_TRUE(WriteFragment(&writer, "more"));
  EXPECT_EQ(ring->LoadWritePosRelaxed(), 3u);
  const DecodedChunk recovered = Decode(ring.get(), first_idx);
  EXPECT_EQ(recovered.fragments, (std::vector<std::string>{"after", "more"}));
  EXPECT_EQ(recovered.payload_flags, kFlagDataLoss);
}

// ---------------------------------------------------------------------------
// Backpressure outcomes.
// ---------------------------------------------------------------------------

TEST(SharedRingBufferWriterTest, FullRingBufferReturnsAtOnce) {
  // A full ring buffer returns kFull without a wait or a reservation.
  test::SharedRingBufferForTesting ring(2, 256);
  SharedRingBufferWriter a = MakeWriter(ring.get(), kWriterA, kBuffer);
  SharedRingBufferWriter b = MakeWriter(ring.get(), kWriterB, kBuffer);

  // Two writers hold both chunks, so the ring buffer is structurally full.
  ASSERT_EQ(a.BeginFragment(1, false).result, BeginFragmentResult::kSuccess);
  ASSERT_EQ(b.BeginFragment(1, false).result, BeginFragmentResult::kSuccess);

  SharedRingBufferWriter c = MakeWriter(ring.get(), 11, kBuffer);
  EXPECT_EQ(c.BeginFragment(1, false).result, BeginFragmentResult::kFull);
  // Nothing was reserved, so a full ring buffer costs no position.
  EXPECT_EQ(ring->LoadWritePosRelaxed(), 2u);
  EXPECT_EQ(c.GetStats().failed_claims, 0u);
}

// The futex wait compares read_pos with the value that the failed call saw.
// So a reader move before the wait ends the wait at once. A missed move would
// block until the timeout. The sleep fallback has no such check. Its first
// sleep is zero, so the retry runs at once.
TEST(SharedRingBufferWriterTest, WaitReturnsAfterEarlierReaderProgress) {
  for (bool use_futex : {true, false}) {
    SCOPED_TRACE(use_futex);
    test::SharedRingBufferForTesting ring(2, 256);
    FillRingBuffer(ring.get());

    SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterB, kBuffer);
    if (!use_futex)
      Internals::DisableWriterFutex(&writer);
    ASSERT_EQ(writer.BeginFragment(1, false).result,
              BeginFragmentResult::kFull);

    ReleaseChunkAtReadPos(ring.get());
    const base::TimeMillis start = base::GetWallTimeMs();
    writer.WaitForReadPosChange(/*timeout_ms=*/30000);
    EXPECT_LT((base::GetWallTimeMs() - start).count(), 30000);
    EXPECT_EQ(Internals::GetNumWritersWaiting(ring.get()), 0u);

    ASSERT_TRUE(WriteFragment(&writer, "recovered"));
    EXPECT_EQ(Decode(ring.get(), ChunkIndex::FromIndex(0)).fragments,
              std::vector<std::string>{"recovered"});
  }
}

// Without a futex, each wait sleeps for the next step of v1's backoff. The
// step stops growing at 100 ms, and it resets when the writer claims a chunk.
// Each sleep is also limited by the timeout, so this test stays fast.
TEST(SharedRingBufferWriterTest, SleepFallbackBackoffIsCapped) {
  test::SharedRingBufferForTesting ring(2, 256);
  FillRingBuffer(ring.get());

  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterB, kBuffer);
  Internals::DisableWriterFutex(&writer);
  // Steps: 0, 8, 72, 584, 4680, 37448 us, then the 100 ms cap.
  for (int i = 0; i < 8; ++i) {
    ASSERT_EQ(writer.BeginFragment(1, false).result,
              BeginFragmentResult::kFull);
    writer.WaitForReadPosChange(/*timeout_ms=*/1);
  }
  EXPECT_EQ(Internals::GetFallbackSleepUs(&writer), 100000u);

  ReleaseChunkAtReadPos(ring.get());
  ASSERT_EQ(writer.BeginFragment(1, false).result,
            BeginFragmentResult::kSuccess);
  EXPECT_EQ(Internals::GetFallbackSleepUs(&writer), 0u);
}

// Without reader progress, the wait ends at its timeout. This timeout must
// fire, so it is short.
TEST(SharedRingBufferWriterTest, WaitEndsAtTimeout) {
  test::SharedRingBufferForTesting ring(2, 256);
  FillRingBuffer(ring.get());

  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterB, kBuffer);
  ASSERT_EQ(writer.BeginFragment(1, false).result, BeginFragmentResult::kFull);
  writer.WaitForReadPosChange(/*timeout_ms=*/1);
  EXPECT_EQ(Internals::GetNumWritersWaiting(ring.get()), 0u);
  EXPECT_EQ(writer.BeginFragment(1, false).result, BeginFragmentResult::kFull);
}

// has_pending_data_loss() stays true until a claimed chunk carries the loss.
// A failed claim does not clear it. TraceWriterV2Impl uses it to keep
// kStallThenDrop from stalling again in a drop episode.
TEST(SharedRingBufferWriterTest, PendingDataLossUntilChunkClaimed) {
  test::SharedRingBufferForTesting ring(2, 256);
  FillRingBuffer(ring.get());

  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterB, kBuffer);
  EXPECT_FALSE(writer.has_pending_data_loss());
  writer.RecordDataLoss();
  EXPECT_TRUE(writer.has_pending_data_loss());
  EXPECT_EQ(writer.BeginFragment(1, false).result, BeginFragmentResult::kFull);
  EXPECT_TRUE(writer.has_pending_data_loss());

  ReleaseChunkAtReadPos(ring.get());
  ASSERT_EQ(writer.BeginFragment(1, false).result,
            BeginFragmentResult::kSuccess);
  EXPECT_FALSE(writer.has_pending_data_loss());
  ASSERT_EQ(writer.EndFragment(0, false), EndFragmentResult::kSuccess);
  EXPECT_EQ(Decode(ring.get(), ChunkIndex::FromIndex(0)).payload_flags,
            kFlagDataLoss);
}

// A chunk pinned by a writer that stopped mid-rewrite is not the same thing as
// a full ring buffer: positions are available, but their chunks cannot be
// acquired.
TEST(SharedRingBufferWriterTest, PinnedChunks) {
  // The writer gives up after num_chunks failed claims. Nobody else reserves
  // here, so those claims land on each physical chunk once and use up the whole
  // reservation window without acquiring anything.
  test::SharedRingBufferForTesting ring(8, 256);
  ASSERT_TRUE(PinAllChunks(ring.get(), kWriterB));
  ASSERT_EQ(ring->LoadWritePosRelaxed(), 0u);

  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);
  EXPECT_EQ(writer.BeginFragment(1, false).result,
            BeginFragmentResult::kClaimFailed);
  EXPECT_EQ(writer.GetStats().failed_claims, ring->num_chunks());
  EXPECT_EQ(ring->LoadWritePosRelaxed(), ring->num_chunks());
}

// Each call makes up to num_chunks claim attempts. A count kept from the
// previous call would stop the next call after one failed claim.
TEST(SharedRingBufferWriterTest, EachCallGetsFullRoundOfClaims) {
  test::SharedRingBufferForTesting ring(4, 256);
  ASSERT_TRUE(PinAllChunks(ring.get(), kWriterB));
  ASSERT_EQ(ring->LoadWritePosRelaxed(), 0u);

  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);
  for (uint32_t round = 1; round <= 3; ++round) {
    SCOPED_TRACE(round);
    EXPECT_EQ(writer.BeginFragment(1, false).result,
              BeginFragmentResult::kClaimFailed);
    EXPECT_EQ(writer.GetStats().failed_claims, round * ring->num_chunks());
    // Play a reader that moves past the unclaimed positions, but cannot free
    // the pinned chunks.
    ring->PublishReadPos(ring->LoadWritePosRelaxed());
  }
}

// ---------------------------------------------------------------------------
// Relocation after a scrape.
// ---------------------------------------------------------------------------

TEST(SharedRingBufferWriterTest, RelocatesUnpublishedFragment) {
  // A scrape moves only the unpublished fragment. The first scrape also
  // allocates the relocation scratch. The second one below reuses it.
  test::SharedRingBufferForTesting ring(4, 512);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);

  const std::vector<std::string> published_fragments = {
      "one", "", std::string(128, 'x'), "three", "four"};
  for (const auto& fragment : published_fragments)
    ASSERT_TRUE(WriteFragment(&writer, fragment));

  // Several published fragments, including a two-byte size and an empty
  // fragment, precede the only completed unpublished fragment this writer can
  // produce.
  const auto range = writer.BeginFragment(6, false);
  ASSERT_EQ(range.result, BeginFragmentResult::kSuccess);
  memcpy(range.begin, "suffix", 6);
  EXPECT_EQ(MarkForRewrite(ring.get(), ChunkIndex::FromIndex(0)),
            published_fragments.size());
  EXPECT_EQ(Decode(ring.get(), ChunkIndex::FromIndex(0)).fragments,
            published_fragments);

  ASSERT_EQ(writer.EndFragment(6, false), EndFragmentResult::kSuccess);
  EXPECT_EQ(writer.GetStats().relocations, 1u);
  EXPECT_EQ(writer.GetStats().fragments_dropped, 0u);

  // The old chunk is acknowledged - the writer says nothing about who gets it
  // next - and only the unpublished fragment moved.
  EXPECT_EQ(
      ChunkStateOf(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0))),
      ChunkState::kRewriteAcknowledged);
  const DecodedChunk replacement = Decode(ring.get(), ChunkIndex::FromIndex(1));
  EXPECT_EQ(replacement.state, ChunkState::kComplete);
  EXPECT_EQ(replacement.writer_id, kWriterA);
  EXPECT_EQ(replacement.target_buffer, kBuffer);
  ASSERT_EQ(replacement.fragments.size(), 1u);
  EXPECT_EQ(replacement.fragments[0], "suffix");
  // Published fragments went out with the chunk's beginning, so the unpublished
  // fragment does not repeat the flags describing it.
  EXPECT_EQ(replacement.payload_flags, 0u);

  // Scrape the replacement too: the newer fragment moves on again.
  const auto again = writer.BeginFragment(5, false);
  ASSERT_EQ(again.result, BeginFragmentResult::kSuccess);
  memcpy(again.begin, "again", 5);
  EXPECT_EQ(MarkForRewrite(ring.get(), ChunkIndex::FromIndex(1)), 1u);
  ASSERT_EQ(writer.EndFragment(5, false), EndFragmentResult::kSuccess);
  EXPECT_EQ(writer.GetStats().relocations, 2u);
  EXPECT_EQ(writer.GetStats().fragments_dropped, 0u);

  EXPECT_EQ(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(1)),
            kRewriteAcknowledgedStateWord);
  const DecodedChunk second = Decode(ring.get(), ChunkIndex::FromIndex(2));
  EXPECT_EQ(second.state, ChunkState::kComplete);
  ASSERT_EQ(second.fragments.size(), 1u);
  EXPECT_EQ(second.fragments[0], "again");
  EXPECT_EQ(second.payload_flags, 0u);
}

TEST(SharedRingBufferWriterTest, RelocationKeepsFlags) {
  // A scrape with no published fragments carries the flags along.
  test::SharedRingBufferForTesting ring(4, 512);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);
  writer.RecordDataLoss();

  const auto range = writer.BeginFragment(4, /*continues_from_prev=*/true);
  ASSERT_EQ(range.result, BeginFragmentResult::kSuccess);
  memcpy(range.begin, "tail", 4);
  // The reader takes nothing: the writer has published no fragment yet.
  EXPECT_EQ(MarkForRewrite(ring.get(), ChunkIndex::FromIndex(0)), 0u);

  ASSERT_EQ(writer.EndFragment(4, false), EndFragmentResult::kSuccess);

  const DecodedChunk replacement = Decode(ring.get(), ChunkIndex::FromIndex(1));
  ASSERT_EQ(replacement.fragments.size(), 1u);
  EXPECT_EQ(replacement.fragments[0], "tail");
  // The reader took no beginning, so both flags describing it travel with the
  // relocated fragment.
  EXPECT_EQ(replacement.payload_flags,
            kFlagContinuesFromPrevChunk | kFlagDataLoss);
}

// An ended zero-byte fragment still needs a replacement and a size varint.
TEST(SharedRingBufferWriterTest, RelocatesZeroLengthFragment) {
  for (bool has_published_fragment : {false, true}) {
    SCOPED_TRACE(has_published_fragment);
    test::SharedRingBufferForTesting ring(4, 512);
    SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);
    writer.RecordDataLoss();
    if (has_published_fragment) {
      ASSERT_TRUE(WriteFragment(&writer, "prefix"));
    }
    ASSERT_EQ(writer.BeginFragment(0, false).result,
              BeginFragmentResult::kSuccess);
    EXPECT_EQ(MarkForRewrite(ring.get(), ChunkIndex::FromIndex(0)),
              has_published_fragment ? 1u : 0u);
    ASSERT_EQ(writer.EndFragment(0, false), EndFragmentResult::kSuccess);

    EXPECT_EQ(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0)),
              kRewriteAcknowledgedStateWord);
    EXPECT_EQ(ring->LoadWritePosRelaxed(), 2u);
    const DecodedChunk replacement =
        Decode(ring.get(), ChunkIndex::FromIndex(1));
    EXPECT_EQ(replacement.state, ChunkState::kComplete);
    ASSERT_EQ(replacement.fragments.size(), 1u);
    EXPECT_TRUE(replacement.fragments[0].empty());
    EXPECT_EQ(replacement.payload_flags,
              has_published_fragment ? 0u : kFlagDataLoss);
    EXPECT_EQ(writer.GetStats().relocations, 1u);
    EXPECT_EQ(writer.GetStats().fragments_dropped, 0u);
  }
}

// A relocation that found no chunk stays pending. RetryRelocation() publishes
// the saved fragment after the reader frees a chunk. The flags are the same as
// for a relocation that succeeds at once.
TEST(SharedRingBufferWriterTest, RetryRelocationAfterReaderFreesChunk) {
  test::SharedRingBufferForTesting ring(2, 512);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);
  const auto range = writer.BeginFragment(4, /*continues_from_prev=*/true);
  ASSERT_EQ(range.result, BeginFragmentResult::kSuccess);
  memcpy(range.begin, "tail", 4);

  // Occupy the ring buffer's only other chunk so no replacement can be had.
  SharedRingBufferWriter blocker = MakeWriter(ring.get(), kWriterB, kBuffer);
  ASSERT_EQ(blocker.BeginFragment(1, false).result,
            BeginFragmentResult::kSuccess);

  EXPECT_EQ(MarkForRewrite(ring.get(), ChunkIndex::FromIndex(0)), 0u);
  EXPECT_EQ(writer.EndFragment(4, /*continues_on_next=*/true),
            EndFragmentResult::kFull);
  const auto no_claim = writer.RetryRelocation();
  EXPECT_EQ(no_claim.result, EndFragmentResult::kFull);
  EXPECT_FALSE(no_claim.acquired_replacement);

  ReleaseChunkAtReadPos(ring.get());
  const auto published = writer.RetryRelocation();
  ASSERT_EQ(published.result, EndFragmentResult::kSuccess);
  EXPECT_TRUE(published.acquired_replacement);
  const DecodedChunk replacement = Decode(ring.get(), ChunkIndex::FromIndex(0));
  EXPECT_EQ(replacement.state, ChunkState::kComplete);
  EXPECT_EQ(replacement.fragments, std::vector<std::string>{"tail"});
  EXPECT_EQ(replacement.payload_flags,
            kFlagContinuesFromPrevChunk | kFlagContinuesOnNextChunk);
  EXPECT_EQ(writer.GetStats().relocations, 1u);
  EXPECT_EQ(writer.GetStats().fragments_dropped, 0u);
}

// One RetryRelocation() call can claim a replacement, lose its publication to
// another rewrite, and then get no chunk. The result still reports the claim,
// because the claim ended one acquisition.
TEST(SharedRingBufferWriterTest, RetryRelocationReportsClaimLostToRewrite) {
  test::SharedRingBufferForTesting ring(2, 512);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);
  const auto range = writer.BeginFragment(4, false);
  ASSERT_EQ(range.result, BeginFragmentResult::kSuccess);
  memcpy(range.begin, "tail", 4);

  // Occupy the ring buffer's only other chunk so no replacement can be had.
  SharedRingBufferWriter blocker = MakeWriter(ring.get(), kWriterB, kBuffer);
  ASSERT_EQ(blocker.BeginFragment(1, false).result,
            BeginFragmentResult::kSuccess);
  EXPECT_EQ(MarkForRewrite(ring.get(), ChunkIndex::FromIndex(0)), 0u);
  EXPECT_EQ(writer.EndFragment(4, false), EndFragmentResult::kFull);

  // The reader frees chunk 0, and the writer claims it as the replacement.
  // The reader requests a rewrite of it before the publication. The blocker
  // still holds chunk 1, so the next claim round gets no chunk.
  ReleaseChunkAtReadPos(ring.get());
  bool rewrite_replacement = true;
  Internals::SetAfterReplacementClaimCallback(&writer, [&] {
    if (!rewrite_replacement)
      return;
    rewrite_replacement = false;
    EXPECT_EQ(MarkForRewrite(ring.get(), ChunkIndex::FromIndex(0)), 0u);
  });
  const auto lost = writer.RetryRelocation();
  EXPECT_EQ(lost.result, EndFragmentResult::kFull);
  EXPECT_TRUE(lost.acquired_replacement);
  EXPECT_EQ(writer.GetStats().relocations, 2u);

  // Free both chunks. The next call publishes the fragment intact.
  blocker.FinishCurrentChunk();
  ReleaseChunkAtReadPos(ring.get());  // Chunk 1, published by the blocker.
  ReleaseChunkAtReadPos(ring.get());  // Chunk 0, acknowledged by the writer.
  const auto published = writer.RetryRelocation();
  ASSERT_EQ(published.result, EndFragmentResult::kSuccess);
  EXPECT_TRUE(published.acquired_replacement);
  const DecodedChunk replacement = Decode(ring.get(), ChunkIndex::FromIndex(1));
  EXPECT_EQ(replacement.fragments, std::vector<std::string>{"tail"});
  EXPECT_EQ(replacement.payload_flags, 0u);
  EXPECT_EQ(writer.GetStats().fragments_dropped, 0u);
}

// A loss flag that the reader did not take moves with the unpublished
// fragment. The writer's data_loss_pending_ is clear by then, but
// has_pending_data_loss() still reports the loss. So kStallThenDrop drops and
// does not start another stall.
TEST(SharedRingBufferWriterTest, PendingRelocationKeepsUnreportedLoss) {
  test::SharedRingBufferForTesting ring(2, 512);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);

  // The loss goes into chunk 0's flags when the writer claims it.
  writer.RecordDataLoss();
  const auto range = writer.BeginFragment(4, false);
  ASSERT_EQ(range.result, BeginFragmentResult::kSuccess);
  memcpy(range.begin, "tail", 4);
  EXPECT_FALSE(writer.has_pending_data_loss());

  // Occupy the ring buffer's only other chunk so no replacement can be had.
  SharedRingBufferWriter blocker = MakeWriter(ring.get(), kWriterB, kBuffer);
  ASSERT_EQ(blocker.BeginFragment(1, false).result,
            BeginFragmentResult::kSuccess);

  // The reader takes nothing, so the flag moves with the unpublished fragment.
  EXPECT_EQ(MarkForRewrite(ring.get(), ChunkIndex::FromIndex(0)), 0u);
  EXPECT_EQ(writer.EndFragment(4, false), EndFragmentResult::kFull);
  EXPECT_TRUE(writer.has_pending_data_loss());

  writer.DropRelocation();
  EXPECT_TRUE(writer.has_pending_data_loss());
  EXPECT_EQ(writer.GetStats().fragments_dropped, 1u);
  EXPECT_EQ(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0)),
            kRewriteAcknowledgedStateWord);

  // The loss is still unreported, so it goes out with the next chunk.
  ReleaseChunkAtReadPos(ring.get());
  ASSERT_TRUE(WriteFragment(&writer, "after loss"));
  EXPECT_EQ(Decode(ring.get(), ChunkIndex::FromIndex(0)).payload_flags,
            kFlagDataLoss);
}

TEST(SharedRingBufferWriterTest, RewriteWithoutUnpublishedFragment) {
  // A scrape with nothing unpublished just acknowledges.
  test::SharedRingBufferForTesting ring(4, 512);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);

  ASSERT_TRUE(WriteFragment(&writer, "only"));
  // Take the chunk back but add nothing, then let the reader scrape it.
  ASSERT_EQ(writer.BeginFragment(1, false).result,
            BeginFragmentResult::kSuccess);
  EXPECT_EQ(MarkForRewrite(ring.get(), ChunkIndex::FromIndex(0)), 1u);

  // Releasing abandons the open fragment. There is nothing left to move.
  EXPECT_EQ(writer.FinishCurrentChunk(), EndFragmentResult::kSuccess);
  EXPECT_EQ(writer.GetStats().fragments_dropped, 0u);
  EXPECT_EQ(
      ChunkStateOf(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0))),
      ChunkState::kRewriteAcknowledged);
  EXPECT_EQ(
      ChunkStateOf(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(1))),
      ChunkState::kFree);
}

TEST(SharedRingBufferWriterTest, NoFragmentToRelocateKeepsDataLossPending) {
  // A scrape that takes nothing, followed by nothing to relocate, leaves the
  // loss flag with the writer for its next chunk.
  test::SharedRingBufferForTesting ring(4, 512);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);
  writer.RecordDataLoss();

  ASSERT_EQ(writer.BeginFragment(4, false).result,
            BeginFragmentResult::kSuccess);
  EXPECT_EQ(MarkForRewrite(ring.get(), ChunkIndex::FromIndex(0)), 0u);

  // Releasing abandons the open fragment, so no fragment can carry the flag.
  EXPECT_EQ(writer.FinishCurrentChunk(), EndFragmentResult::kSuccess);
  EXPECT_EQ(
      ChunkStateOf(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0))),
      ChunkState::kRewriteAcknowledged);

  ASSERT_TRUE(WriteFragment(&writer, "next"));
  EXPECT_EQ(Decode(ring.get(), ChunkIndex::FromIndex(1)).payload_flags,
            kFlagDataLoss);
}

// Acknowledging happens before the writer looks for replacement capacity. The
// other order would leave the old chunk occupied exactly when the ring buffer
// is full, so every later traversal of it would burn a position.
TEST(SharedRingBufferWriterTest, RelocationDrop) {
  for (const std::string& unpublished_fragment :
       {std::string("lost"), std::string()}) {
    SCOPED_TRACE(unpublished_fragment);
    test::SharedRingBufferForTesting ring(2, 512);
    SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);

    ASSERT_TRUE(WriteFragment(&writer, "published"));

    // Occupy the ring buffer's only other chunk so no replacement can be had.
    SharedRingBufferWriter blocker = MakeWriter(ring.get(), kWriterB, kBuffer);
    ASSERT_EQ(blocker.BeginFragment(1, false).result,
              BeginFragmentResult::kSuccess);

    const auto range = writer.BeginFragment(4, false);
    ASSERT_EQ(range.result, BeginFragmentResult::kSuccess);
    if (!unpublished_fragment.empty())
      memcpy(range.begin, unpublished_fragment.data(),
             unpublished_fragment.size());
    EXPECT_EQ(MarkForRewrite(ring.get(), ChunkIndex::FromIndex(0)), 1u);

    // No chunk is free, so the relocation stays pending until it is dropped.
    EXPECT_EQ(writer.EndFragment(
                  static_cast<uint32_t>(unpublished_fragment.size()), false),
              EndFragmentResult::kFull);
    writer.DropRelocation();
    EXPECT_EQ(writer.GetStats().fragments_dropped, 1u);
    EXPECT_EQ(writer.GetStats().relocations, 1u);
    // The old chunk is acknowledged and therefore reclaimable by the reader,
    // even though the data did not survive.
    EXPECT_EQ(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0)),
              kRewriteAcknowledgedStateWord);

    // Another failed acquisition must leave the loss pending.
    EXPECT_EQ(writer.BeginFragment(1, false).result,
              BeginFragmentResult::kFull);
    EXPECT_EQ(writer.GetStats().fragments_dropped, 1u);

    // The gap is reported on the next chunk this writer manages to publish.
    uint32_t observed = 0;
    ASSERT_TRUE(ring->TryReleaseRewriteAcknowledgedChunkAsFree(0, &observed));
    ring->PublishReadPos(1);
    ASSERT_TRUE(WriteFragment(&writer, "next"));
    EXPECT_EQ(Decode(ring.get(), ChunkIndex::FromIndex(0)).payload_flags,
              kFlagDataLoss);
    EXPECT_EQ(writer.GetStats().fragments_dropped, 1u);
  }
}

TEST(SharedRingBufferWriterTest, FinishForgetsPublishedChunk) {
  for (bool abandon_open_fragment : {false, true}) {
    SCOPED_TRACE(abandon_open_fragment);
    test::SharedRingBufferForTesting ring(4, 512);
    SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);
    ASSERT_TRUE(WriteFragment(&writer, "before"));
    if (abandon_open_fragment) {
      ASSERT_EQ(writer.BeginFragment(1, false).result,
                BeginFragmentResult::kSuccess);
    }
    ASSERT_EQ(writer.FinishCurrentChunk(), EndFragmentResult::kSuccess);
    ASSERT_TRUE(WriteFragment(&writer, "after"));
    EXPECT_EQ(ring->LoadWritePosRelaxed(), 2u);
    EXPECT_EQ(Decode(ring.get(), ChunkIndex::FromIndex(0)).fragments,
              std::vector<std::string>{"before"});
    EXPECT_EQ(Decode(ring.get(), ChunkIndex::FromIndex(1)).fragments,
              std::vector<std::string>{"after"});
  }
}

TEST(SharedRingBufferWriterTest, DestructorPublishes) {
  // The destructor publishes whatever is held.
  test::SharedRingBufferForTesting ring(4, 512);
  {
    SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);
    const auto range = writer.BeginFragment(4, false);
    ASSERT_EQ(range.result, BeginFragmentResult::kSuccess);
    memcpy(range.begin, "kept", 4);
    ASSERT_EQ(writer.EndFragment(4, false), EndFragmentResult::kSuccess);

    // A second fragment is opened and never closed: it is abandoned.
    ASSERT_EQ(writer.BeginFragment(4, false).result,
              BeginFragmentResult::kSuccess);
  }
  const DecodedChunk decoded = Decode(ring.get(), ChunkIndex::FromIndex(0));
  EXPECT_EQ(decoded.state, ChunkState::kComplete);
  ASSERT_EQ(decoded.fragments.size(), 1u);
  EXPECT_EQ(decoded.fragments[0], "kept");
}

}  // namespace
}  // namespace perfetto::tracing_v2
