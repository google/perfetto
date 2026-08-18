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

#include "src/tracing/v2/ring_writer.h"

#include <stdint.h>
#include <string.h>

#include <memory>
#include <string>
#include <vector>

#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/tracing_v2_abi.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::tracing_v2 {
namespace {

constexpr WriterID kWriterA = 7;
constexpr WriterID kWriterB = 8;
constexpr BufferID kBuffer = 0x1234;

// A minimal, independent decoder for what a chunk holds. It deliberately does
// not go through ChunkReader, so that a writer test cannot pass because the
// reader shares the same misunderstanding.
struct DecodedChunk {
  ChunkState state = ChunkState::kFreeForWrap;
  WriterID writer_id = 0;
  BufferID target_buffer = 0;
  uint32_t payload_flags = 0;
  std::vector<std::string> fragments;
};

DecodedChunk Decode(SharedRingBuffer* ring, uint32_t chunk_index) {
  const uint8_t* chunk = ring->chunk_at(chunk_index);
  const uint32_t word = ring->LoadStateAcquire(chunk_index);
  DecodedChunk decoded;
  decoded.state = StateOf(word);
  if (!IsDataBearing(decoded.state))
    return decoded;

  decoded.writer_id = WriterIdOf(word);
  decoded.target_buffer = LoadTargetBuffer(chunk);
  decoded.payload_flags = PayloadFlagsOf(word);

  const uint32_t width = ring->fragment_size_width();
  uint32_t offset = kTargetBufferPayloadOffset;
  for (uint32_t i = 0; i < NumFragmentsOf(word); ++i) {
    const uint32_t size = LoadFragmentSize(
        chunk + FragmentSizeEntryOffset(ring->chunk_size(), width, i), width);
    decoded.fragments.emplace_back(
        reinterpret_cast<const char*>(chunk + offset), size);
    offset += size;
  }
  return decoded;
}

// Writes one fragment holding |bytes| and publishes it.
RingWriter::Outcome WriteFragment(RingWriter* writer,
                                  const std::string& bytes,
                                  bool continues_from_prev = false,
                                  bool continues_on_next = false) {
  const RingWriter::FragmentSpan span = writer->OpenFragment(
      static_cast<uint32_t>(bytes.size()), continues_from_prev);
  if (span.outcome != RingWriter::Outcome::kOk)
    return span.outcome;
  memcpy(span.begin, bytes.data(), bytes.size());
  return writer->CloseFragment(static_cast<uint32_t>(bytes.size()),
                               continues_on_next);
}

// Plays the reader's part of the scrape: marks whatever the writer currently
// holds as rewrite-requested and reports what the marked prefix was.
uint32_t MarkForRewrite(SharedRingBuffer* ring, uint32_t chunk_index) {
  uint32_t observed = ring->LoadStateAcquire(chunk_index);
  EXPECT_EQ(StateOf(observed), ChunkState::kAcquired);
  const uint32_t taken = NumFragmentsOf(observed);
  EXPECT_TRUE(ring->TryMarkForRewrite(chunk_index, &observed));
  return taken;
}

// ---------------------------------------------------------------------------
// Fragments and the directory.
// ---------------------------------------------------------------------------

TEST(RingWriterTest, WritesPayloadUpAndSizesDown) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  RingWriter writer(ring.get(), kWriterA, kBuffer,
                    BufferExhaustedPolicy::kDrop);

  ASSERT_EQ(WriteFragment(&writer, std::string(5, 'a')),
            RingWriter::Outcome::kOk);
  ASSERT_EQ(WriteFragment(&writer, std::string(200, 'b')),
            RingWriter::Outcome::kOk);
  ASSERT_EQ(WriteFragment(&writer, std::string(3, 'c')),
            RingWriter::Outcome::kOk);

  // A 256-byte target-buffer chunk, byte for byte.
  const uint8_t* chunk = ring->chunk_at(0);
  EXPECT_EQ(chunk[255], 5u);
  EXPECT_EQ(chunk[254], 200u);
  EXPECT_EQ(chunk[253], 3u);

  const DecodedChunk decoded = Decode(ring.get(), 0);
  EXPECT_EQ(decoded.state, ChunkState::kComplete);
  EXPECT_EQ(decoded.writer_id, kWriterA);
  EXPECT_EQ(decoded.target_buffer, kBuffer);
  ASSERT_EQ(decoded.fragments.size(), 3u);
  EXPECT_EQ(decoded.fragments[0], std::string(5, 'a'));
  EXPECT_EQ(decoded.fragments[1], std::string(200, 'b'));
  EXPECT_EQ(decoded.fragments[2], std::string(3, 'c'));
}

TEST(RingWriterTest, FragmentSizesAtTheInterestingBoundaries) {
  // A 512-byte chunk uses two-byte entries, so a single fragment can be larger
  // than 255 bytes and the sizes straddle the one-byte boundary.
  auto ring = SharedRingBuffer::Create(8, 512);
  ASSERT_NE(ring, nullptr);
  RingWriter writer(ring.get(), kWriterA, kBuffer,
                    BufferExhaustedPolicy::kDrop);

  const uint32_t kSizes[] = {0, 1, 127, 128, 255, 256};
  std::vector<std::string> expected;
  for (uint32_t size : kSizes) {
    const std::string bytes(size, static_cast<char>('A' + (size % 26)));
    ASSERT_EQ(WriteFragment(&writer, bytes), RingWriter::Outcome::kOk) << size;
    expected.push_back(bytes);
  }

  // 0 + 1 + 127 + 128 + 255 + 256 = 767 bytes, which does not fit in one
  // 512-byte chunk, so the writer moved on part way through. Walk every chunk
  // it used and check the fragments come out in order.
  std::vector<std::string> seen;
  for (uint32_t i = 0; i < ring->num_chunks(); ++i) {
    for (const std::string& fragment : Decode(ring.get(), i).fragments)
      seen.push_back(fragment);
  }
  EXPECT_EQ(seen, expected);
}

TEST(RingWriterTest, LargestFragmentFitsExactlyAndOneMoreByteDoesNot) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  // 256 - 6 header bytes - 1 directory byte.
  constexpr uint32_t kLargest = 249;

  RingWriter writer(ring.get(), kWriterA, kBuffer,
                    BufferExhaustedPolicy::kDrop);
  const RingWriter::FragmentSpan span = writer.OpenFragment(kLargest, false);
  ASSERT_EQ(span.outcome, RingWriter::Outcome::kOk);
  EXPECT_EQ(static_cast<uint32_t>(span.end - span.begin), kLargest);
  memset(span.begin, 'z', kLargest);
  ASSERT_EQ(writer.CloseFragment(kLargest, false), RingWriter::Outcome::kOk);

  // The payload and the directory have met: the chunk cannot take another
  // fragment, not even an empty one.
  EXPECT_EQ(writer.AvailableForNextFragment(), 0u);
  ASSERT_EQ(Decode(ring.get(), 0).fragments.size(), 1u);
  EXPECT_EQ(Decode(ring.get(), 0).fragments[0].size(), kLargest);

  // One byte more than any chunk of this geometry could ever hold is a caller
  // bug, not backpressure, and says so.
  RingWriter other(ring.get(), kWriterB, kBuffer, BufferExhaustedPolicy::kDrop);
  EXPECT_EQ(other.OpenFragment(kLargest + 1, false).outcome,
            RingWriter::Outcome::kTooLarge);
}

TEST(RingWriterTest, ChunkClosesAt255FragmentsEvenWithSpaceLeft) {
  auto ring = SharedRingBuffer::Create(4, 32768);
  ASSERT_NE(ring, nullptr);
  RingWriter writer(ring.get(), kWriterA, kBuffer,
                    BufferExhaustedPolicy::kDrop);

  for (uint32_t i = 0; i < kMaxFragmentsPerChunk; ++i)
    ASSERT_EQ(WriteFragment(&writer, ""), RingWriter::Outcome::kOk) << i;

  const DecodedChunk first = Decode(ring.get(), 0);
  EXPECT_EQ(first.fragments.size(), kMaxFragmentsPerChunk);
  // Thousands of payload bytes are still free; the eight-bit count is what
  // ended the chunk.
  EXPECT_EQ(writer.AvailableForNextFragment(), 0u);

  ASSERT_EQ(WriteFragment(&writer, "x"), RingWriter::Outcome::kOk);
  const DecodedChunk second = Decode(ring.get(), 1);
  ASSERT_EQ(second.fragments.size(), 1u);
  EXPECT_EQ(second.fragments[0], "x");
}

// ---------------------------------------------------------------------------
// Chunk reuse and the payload flags.
// ---------------------------------------------------------------------------

TEST(RingWriterTest, ReusesItsOwnCompleteChunkWhileItCanTakeMore) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  RingWriter writer(ring.get(), kWriterA, kBuffer,
                    BufferExhaustedPolicy::kDrop);

  ASSERT_EQ(WriteFragment(&writer, "one"), RingWriter::Outcome::kOk);
  ASSERT_EQ(WriteFragment(&writer, "two"), RingWriter::Outcome::kOk);

  // Both fragments are in the same physical chunk, and only one position was
  // consumed.
  EXPECT_EQ(ring->LoadWritePosRelaxed(), 1u);
  const DecodedChunk decoded = Decode(ring.get(), 0);
  ASSERT_EQ(decoded.fragments.size(), 2u);
  EXPECT_EQ(decoded.fragments[0], "one");
  EXPECT_EQ(decoded.fragments[1], "two");
  EXPECT_EQ(StateOf(ring->LoadStateAcquire(1)), ChunkState::kFreeForWrap);
}

TEST(RingWriterTest, ReuseLosesToTheReaderReclaimingTheChunk) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  RingWriter writer(ring.get(), kWriterA, kBuffer,
                    BufferExhaustedPolicy::kDrop);

  ASSERT_EQ(WriteFragment(&writer, "one"), RingWriter::Outcome::kOk);

  // The reader consumes the Complete chunk before the writer takes it back.
  uint32_t observed = ring->LoadStateAcquire(0);
  ASSERT_EQ(StateOf(observed), ChunkState::kComplete);
  ASSERT_TRUE(ring->TryReclaimComplete(0, &observed));
  ring->PublishReadPos(1);

  // The writer's reuse fails; it drops its handle and goes for a fresh chunk
  // rather than writing behind the reader.
  ASSERT_EQ(WriteFragment(&writer, "two"), RingWriter::Outcome::kOk);
  EXPECT_EQ(StateOf(ring->LoadStateAcquire(0)), ChunkState::kFreeForWrap);
  const DecodedChunk decoded = Decode(ring.get(), 1);
  ASSERT_EQ(decoded.fragments.size(), 1u);
  EXPECT_EQ(decoded.fragments[0], "two");
}

// A chunk published with "continues on next chunk" is never reused. Without
// that rule a later scrape could take a prefix ending in the middle of a
// packet.
TEST(RingWriterTest, ChunkCarryingContinuesOnNextIsNeverReused) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  RingWriter writer(ring.get(), kWriterA, kBuffer,
                    BufferExhaustedPolicy::kDrop);

  ASSERT_EQ(WriteFragment(&writer, "head", /*continues_from_prev=*/false,
                          /*continues_on_next=*/true),
            RingWriter::Outcome::kOk);
  ASSERT_EQ(WriteFragment(&writer, "tail", /*continues_from_prev=*/true,
                          /*continues_on_next=*/false),
            RingWriter::Outcome::kOk);

  const DecodedChunk first = Decode(ring.get(), 0);
  EXPECT_EQ(first.payload_flags, kFlagContinuesOnNextChunk);
  ASSERT_EQ(first.fragments.size(), 1u);
  EXPECT_EQ(first.fragments[0], "head");

  const DecodedChunk second = Decode(ring.get(), 1);
  EXPECT_EQ(second.payload_flags, kFlagContinuesFromPrevChunk);
  ASSERT_EQ(second.fragments.size(), 1u);
  EXPECT_EQ(second.fragments[0], "tail");
}

TEST(RingWriterTest, DataLossIsReportedOnTheNextChunkAndOnlyOnce) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  RingWriter writer(ring.get(), kWriterA, kBuffer,
                    BufferExhaustedPolicy::kDrop);

  writer.RecordDataLoss();
  ASSERT_EQ(WriteFragment(&writer, "after the gap"), RingWriter::Outcome::kOk);
  EXPECT_EQ(Decode(ring.get(), 0).payload_flags, kFlagDataLoss);

  // The chunk after it describes no gap of its own.
  ASSERT_EQ(WriteFragment(&writer, std::string(500, 'x')),
            RingWriter::Outcome::kOk);
  EXPECT_EQ(Decode(ring.get(), 1).payload_flags, 0u);
}

// ---------------------------------------------------------------------------
// Backpressure outcomes.
// ---------------------------------------------------------------------------

TEST(RingWriterTest, DropPolicyReportsFullWithoutBlocking) {
  auto ring = SharedRingBuffer::Create(2, 256);
  ASSERT_NE(ring, nullptr);
  RingWriter a(ring.get(), kWriterA, kBuffer, BufferExhaustedPolicy::kDrop);
  RingWriter b(ring.get(), kWriterB, kBuffer, BufferExhaustedPolicy::kDrop);

  // Two writers hold both chunks, so the ring is structurally full.
  ASSERT_EQ(a.OpenFragment(1, false).outcome, RingWriter::Outcome::kOk);
  ASSERT_EQ(b.OpenFragment(1, false).outcome, RingWriter::Outcome::kOk);

  RingWriter c(ring.get(), 11, kBuffer, BufferExhaustedPolicy::kDrop);
  EXPECT_EQ(c.OpenFragment(1, false).outcome, RingWriter::Outcome::kFull);
  // Nothing was reserved, so a full ring costs no position.
  EXPECT_EQ(ring->LoadWritePosRelaxed(), 2u);
  EXPECT_EQ(c.num_holes(), 0u);
}

// A chunk pinned by a writer that stopped mid-rewrite is not the same thing as
// a full ring: positions are available, they just cannot be claimed.
TEST(RingWriterTest, PinnedChunksReportNoChunkAvailableNotFull) {
  // Eight chunks, so the writer runs out of claim budget long before the ring
  // could be reported full.
  auto ring = SharedRingBuffer::Create(8, 256);
  ASSERT_NE(ring, nullptr);

  // Pin every chunk in RewriteRequested, which only its owner may leave.
  // Claiming them directly leaves write_pos at zero, so there is capacity for
  // every reservation the writer below makes.
  for (uint32_t i = 0; i < ring->num_chunks(); ++i) {
    const uint32_t acquired = MakeDataBearingWord(
        ChunkState::kAcquired, ChunkFormat::kTargetBuffer, 0, 0, kWriterB);
    ASSERT_TRUE(ring->TryClaim(i, acquired));
    uint32_t observed = acquired;
    ASSERT_TRUE(ring->TryMarkForRewrite(i, &observed));
  }
  ASSERT_EQ(ring->LoadWritePosRelaxed(), 0u);

  RingWriter writer(ring.get(), kWriterA, kBuffer,
                    BufferExhaustedPolicy::kDrop);
  EXPECT_EQ(writer.OpenFragment(1, false).outcome,
            RingWriter::Outcome::kNoChunkAvailable);
  // Every failed claim burned exactly one position and none of them was
  // retried, so write_pos moved by the number of holes and no further.
  EXPECT_GT(writer.num_holes(), 0u);
  EXPECT_EQ(ring->LoadWritePosRelaxed(), writer.num_holes());
}

// The same thing, but with a ring small enough that the holes the writer makes
// fill it. Its own failed claims are what make the ring look full, and the
// positions they burned are ones only the reader can resolve - so the answer is
// still "no chunk available", not "full", whatever the exhaustion policy says.
//
// For a stalling writer that also means not waiting: nothing has told the
// reader to run, because the nudge happens after this call returns. Without
// that this test parks for kMaxStallSlices * kStallSliceMs and then aborts.
TEST(RingWriterTest, HolesThatFillTheRingAreNotReportedAsFull) {
  for (BufferExhaustedPolicy policy :
       {BufferExhaustedPolicy::kDrop, BufferExhaustedPolicy::kStall}) {
    auto ring = SharedRingBuffer::Create(2, 256);
    ASSERT_NE(ring, nullptr);

    // Both physical chunks pinned by a writer that stopped mid-rewrite, with
    // the cursors untouched: every position the writer below reserves maps onto
    // one of them and cannot be claimed.
    for (uint32_t i = 0; i < ring->num_chunks(); ++i) {
      const uint32_t acquired = MakeDataBearingWord(
          ChunkState::kAcquired, ChunkFormat::kTargetBuffer, 0, 0, kWriterB);
      ASSERT_TRUE(ring->TryClaim(i, acquired));
      uint32_t observed = acquired;
      ASSERT_TRUE(ring->TryMarkForRewrite(i, &observed));
    }
    ASSERT_EQ(ring->LoadWritePosRelaxed(), 0u);

    RingWriter writer(ring.get(), kWriterA, kBuffer, policy);
    EXPECT_EQ(writer.OpenFragment(1, false).outcome,
              RingWriter::Outcome::kNoChunkAvailable);
    // Two positions burned, which is the whole ring: the third reservation is
    // the one that found it full and reported the holes instead.
    EXPECT_EQ(writer.num_holes(), 2u);
    EXPECT_EQ(ring->LoadWritePosRelaxed(), 2u);
    // Nobody parked on read_pos.
    EXPECT_EQ(ring->num_writers_waiting_for_testing(), 0u);
  }
}

// ---------------------------------------------------------------------------
// Relocation after a scrape.
// ---------------------------------------------------------------------------

TEST(RingWriterTest, ScrapeMovesOnlyTheUnpublishedSuffix) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  RingWriter writer(ring.get(), kWriterA, kBuffer,
                    BufferExhaustedPolicy::kDrop);

  ASSERT_EQ(WriteFragment(&writer, "published-one"), RingWriter::Outcome::kOk);
  ASSERT_EQ(WriteFragment(&writer, "published-two"), RingWriter::Outcome::kOk);

  // The writer takes the chunk back and fills a third fragment. The reader
  // arrives while it is inside and takes the two published ones.
  const RingWriter::FragmentSpan span = writer.OpenFragment(6, false);
  ASSERT_EQ(span.outcome, RingWriter::Outcome::kOk);
  memcpy(span.begin, "suffix", 6);
  EXPECT_EQ(MarkForRewrite(ring.get(), 0), 2u);

  ASSERT_EQ(writer.CloseFragment(6, false), RingWriter::Outcome::kOk);
  EXPECT_EQ(writer.num_relocations(), 1u);
  EXPECT_EQ(writer.num_fragments_dropped(), 0u);

  // The old chunk is acknowledged - the writer says nothing about who gets it
  // next - and only the suffix moved.
  EXPECT_EQ(StateOf(ring->LoadStateAcquire(0)), ChunkState::kAcknowledged);
  const DecodedChunk replacement = Decode(ring.get(), 1);
  EXPECT_EQ(replacement.state, ChunkState::kComplete);
  EXPECT_EQ(replacement.writer_id, kWriterA);
  EXPECT_EQ(replacement.target_buffer, kBuffer);
  ASSERT_EQ(replacement.fragments.size(), 1u);
  EXPECT_EQ(replacement.fragments[0], "suffix");
  // A non-empty prefix went out with the chunk's beginning, so the suffix does
  // not repeat the flags describing it.
  EXPECT_EQ(replacement.payload_flags, 0u);
}

TEST(RingWriterTest, ScrapeWithNoPublishedPrefixCarriesTheFlagsAlong) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  RingWriter writer(ring.get(), kWriterA, kBuffer,
                    BufferExhaustedPolicy::kDrop);
  writer.RecordDataLoss();

  const RingWriter::FragmentSpan span =
      writer.OpenFragment(4, /*continues_from_prev=*/true);
  ASSERT_EQ(span.outcome, RingWriter::Outcome::kOk);
  memcpy(span.begin, "tail", 4);
  // The reader takes nothing: the writer has published no fragment yet.
  EXPECT_EQ(MarkForRewrite(ring.get(), 0), 0u);

  ASSERT_EQ(writer.CloseFragment(4, false), RingWriter::Outcome::kOk);

  const DecodedChunk replacement = Decode(ring.get(), 1);
  ASSERT_EQ(replacement.fragments.size(), 1u);
  EXPECT_EQ(replacement.fragments[0], "tail");
  // The reader took no beginning, so both flags describing it travel with the
  // relocated suffix.
  EXPECT_EQ(replacement.payload_flags,
            kFlagContinuesFromPrevChunk | kFlagDataLoss);
}

TEST(RingWriterTest, ScrapeWithNothingUnpublishedJustAcknowledges) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  RingWriter writer(ring.get(), kWriterA, kBuffer,
                    BufferExhaustedPolicy::kDrop);

  ASSERT_EQ(WriteFragment(&writer, "only"), RingWriter::Outcome::kOk);
  // Take the chunk back but add nothing, then let the reader scrape it.
  ASSERT_EQ(writer.OpenFragment(1, false).outcome, RingWriter::Outcome::kOk);
  EXPECT_EQ(MarkForRewrite(ring.get(), 0), 1u);

  // Releasing abandons the open fragment; there is nothing left to move.
  EXPECT_EQ(writer.Release(), RingWriter::Outcome::kOk);
  EXPECT_EQ(writer.num_fragments_dropped(), 0u);
  EXPECT_EQ(StateOf(ring->LoadStateAcquire(0)), ChunkState::kAcknowledged);
  EXPECT_EQ(StateOf(ring->LoadStateAcquire(1)), ChunkState::kFreeForWrap);
}

// Acknowledging happens before the writer looks for replacement capacity. The
// other order would leave the old chunk occupied exactly when the ring is full,
// so every later traversal of it would burn a position.
TEST(RingWriterTest, RelocationWithNoCapacityDropsButFreesTheOldChunk) {
  auto ring = SharedRingBuffer::Create(2, 512);
  ASSERT_NE(ring, nullptr);
  RingWriter writer(ring.get(), kWriterA, kBuffer,
                    BufferExhaustedPolicy::kDrop);

  ASSERT_EQ(WriteFragment(&writer, "published"), RingWriter::Outcome::kOk);

  // Occupy the ring's only other chunk so no replacement can be had.
  RingWriter blocker(ring.get(), kWriterB, kBuffer,
                     BufferExhaustedPolicy::kDrop);
  ASSERT_EQ(blocker.OpenFragment(1, false).outcome, RingWriter::Outcome::kOk);

  const RingWriter::FragmentSpan span = writer.OpenFragment(4, false);
  ASSERT_EQ(span.outcome, RingWriter::Outcome::kOk);
  memcpy(span.begin, "lost", 4);
  EXPECT_EQ(MarkForRewrite(ring.get(), 0), 1u);

  EXPECT_EQ(writer.CloseFragment(4, false),
            RingWriter::Outcome::kRelocationDropped);
  EXPECT_EQ(writer.num_fragments_dropped(), 1u);
  // The old chunk is acknowledged and therefore reclaimable by the reader,
  // even though the data did not survive.
  EXPECT_EQ(ring->LoadStateAcquire(0), kAcknowledgedWord);

  // The gap is reported on the next chunk this writer manages to publish.
  ASSERT_TRUE(ring->TryReclaimAcknowledged(0));
  ring->PublishReadPos(1);
  ASSERT_EQ(WriteFragment(&writer, "next"), RingWriter::Outcome::kOk);
  EXPECT_EQ(Decode(ring.get(), 0).payload_flags, kFlagDataLoss);
}

TEST(RingWriterTest, DestructorPublishesWhateverIsHeld) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  {
    RingWriter writer(ring.get(), kWriterA, kBuffer,
                      BufferExhaustedPolicy::kDrop);
    const RingWriter::FragmentSpan span = writer.OpenFragment(4, false);
    ASSERT_EQ(span.outcome, RingWriter::Outcome::kOk);
    memcpy(span.begin, "kept", 4);
    ASSERT_EQ(writer.CloseFragment(4, false), RingWriter::Outcome::kOk);

    // A second fragment is opened and never closed: it is abandoned.
    ASSERT_EQ(writer.OpenFragment(4, false).outcome, RingWriter::Outcome::kOk);
  }
  const DecodedChunk decoded = Decode(ring.get(), 0);
  EXPECT_EQ(decoded.state, ChunkState::kComplete);
  ASSERT_EQ(decoded.fragments.size(), 1u);
  EXPECT_EQ(decoded.fragments[0], "kept");
}

}  // namespace
}  // namespace perfetto::tracing_v2
