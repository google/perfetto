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

#include "perfetto/tracing/buffer_exhausted_policy.h"
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
using test::WriteFragment;

constexpr WriterID kWriterA = 7;
constexpr WriterID kWriterB = 8;
constexpr BufferID kBuffer = 0x1234;

class CountingSharedRingBufferWriterDelegate
    : public SharedRingBufferWriter::Delegate {
 public:
  void NotifyReader() override { ++num_notifications; }

  uint32_t num_notifications = 0;
};

// Plays a reader that frees one chunk whenever the writer asks for one: the
// chunk at read_pos, which must be finished with (Complete or
// RewriteAcknowledged), becomes Free and read_pos moves past it.
class ReleasingSharedRingBufferWriterDelegate
    : public SharedRingBufferWriter::Delegate {
 public:
  explicit ReleasingSharedRingBufferWriterDelegate(SharedRingBuffer* ring)
      : ring_(ring) {}

  void NotifyReader() override {
    ++num_notifications;
    const uint32_t read_pos = Internals::GetReadPos(ring_);
    const auto chunk_idx =
        ChunkIndex::FromPosition(read_pos, ring_->num_chunks());
    uint32_t observed = ring_->LoadChunkStateWord(chunk_idx);
    if (ChunkStateOf(observed) == ChunkState::kRewriteAcknowledged) {
      ASSERT_TRUE(
          ring_->TryReleaseRewriteAcknowledgedChunkAsFree(read_pos, &observed));
    } else {
      ASSERT_EQ(ChunkStateOf(observed), ChunkState::kComplete);
      ASSERT_TRUE(ring_->TryReleaseCompleteChunkAsFree(read_pos, &observed));
    }
    ring_->PublishReadPos(read_pos + 1);
  }

  uint32_t num_notifications = 0;

 private:
  SharedRingBuffer* const ring_;
};

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
  const uint32_t word = ring->LoadChunkStateWord(chunk_idx);
  DecodedChunk decoded;
  decoded.state = ChunkStateOf(word);
  if (!HasDataFields(decoded.state))
    return decoded;

  decoded.writer_id = WriterIDOf(word);
  decoded.target_buffer = LoadTargetBufferID(chunk);
  decoded.payload_flags = PayloadFlagsOf(word);

  const uint8_t* sizes_cursor = chunk + ring->chunk_size();
  uint32_t offset = kTargetBufferPayloadOffset;
  for (uint32_t i = 0; i < NumFragmentsOf(word); ++i) {
    const auto size = ReadFragmentSizeReversed(
        chunk + kTargetBufferPayloadOffset, &sizes_cursor);
    EXPECT_TRUE(size);
    if (!size)
      return decoded;
    decoded.fragments.emplace_back(
        reinterpret_cast<const char*>(chunk + offset), *size);
    offset += *size;
  }
  return decoded;
}

// Plays the reader's part of the scrape: marks whatever the writer currently
// holds as rewrite-requested and reports what the marked prefix was.
uint32_t MarkForRewrite(SharedRingBuffer* ring, ChunkIndex chunk_idx) {
  uint32_t observed = ring->LoadChunkStateWord(chunk_idx);
  EXPECT_EQ(ChunkStateOf(observed), ChunkState::kBeingWritten);
  const uint32_t taken = NumFragmentsOf(observed);
  EXPECT_TRUE(ring->TryRequestRewrite(chunk_idx, &observed));
  return taken;
}

// ---------------------------------------------------------------------------
// Fragments and their sizes.
// ---------------------------------------------------------------------------

TEST(SharedRingBufferWriterTest, PayloadUpSizesDown) {
  // Payload grows up from the header; sizes grow down from the end.
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
  // The largest fragment fits exactly; one more byte is too large.
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
  // A large chunk holds a fragment longer than 65535 bytes.
  constexpr uint32_t kBigChunk = 128 * 1024;
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
    EXPECT_EQ(ring->LoadWritePos(), kept_chunk ? 1u : 2u);
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
  EXPECT_EQ(ring->LoadWritePos(), 1u);
  const DecodedChunk decoded = Decode(ring.get(), ChunkIndex::FromIndex(0));
  ASSERT_EQ(decoded.fragments.size(), 2u);
  EXPECT_EQ(decoded.fragments[0], "one");
  EXPECT_EQ(decoded.fragments[1], "two");
  EXPECT_EQ(ChunkStateOf(ring->LoadChunkStateWord(ChunkIndex::FromIndex(1))),
            ChunkState::kFree);
}

TEST(SharedRingBufferWriterTest, ReuseLosesToReclaim) {
  // Reuse loses to the reader reclaiming the chunk first.
  test::SharedRingBufferForTesting ring(4, 512);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);

  ASSERT_TRUE(WriteFragment(&writer, "one"));

  // The reader consumes the Complete chunk before the writer takes it back.
  uint32_t observed = ring->LoadChunkStateWord(ChunkIndex::FromIndex(0));
  ASSERT_EQ(ChunkStateOf(observed), ChunkState::kComplete);
  ASSERT_TRUE(ring->TryReleaseCompleteChunkAsFree(0, &observed));
  ring->PublishReadPos(1);

  // The writer's reuse fails; it drops its handle and goes for a fresh chunk
  // rather than writing behind the reader.
  ASSERT_TRUE(WriteFragment(&writer, "two"));
  EXPECT_EQ(ChunkStateOf(ring->LoadChunkStateWord(ChunkIndex::FromIndex(0))),
            ChunkState::kFree);
  const DecodedChunk decoded = Decode(ring.get(), ChunkIndex::FromIndex(1));
  ASSERT_EQ(decoded.fragments.size(), 1u);
  EXPECT_EQ(decoded.fragments[0], "two");
}

// A chunk published with "continues on next chunk" is never reused. Without
// that rule a later scrape could take a prefix ending in the middle of a
// packet.
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

// A loss recorded while the writer still holds a reusable Complete chunk must
// not put the post-loss fragment into that chunk: the flag would then only
// appear on a later one.
TEST(SharedRingBufferWriterTest, DataLossSkipsCachedChunk) {
  test::SharedRingBufferForTesting ring(4, 512);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);

  ASSERT_TRUE(WriteFragment(&writer, "before"));
  writer.RecordDataLoss();
  ASSERT_TRUE(WriteFragment(&writer, "after"));

  // The pre-loss fragment stays alone in chunk 0. The post-loss fragment opened
  // a new position, and its chunk is the one reporting the gap.
  EXPECT_EQ(ring->LoadWritePos(), 2u);
  const DecodedChunk before = Decode(ring.get(), ChunkIndex::FromIndex(0));
  ASSERT_EQ(before.fragments.size(), 1u);
  EXPECT_EQ(before.fragments[0], "before");
  EXPECT_EQ(before.payload_flags, 0u);
  const DecodedChunk after = Decode(ring.get(), ChunkIndex::FromIndex(1));
  ASSERT_EQ(after.fragments.size(), 1u);
  EXPECT_EQ(after.fragments[0], "after");
  EXPECT_EQ(after.payload_flags, kFlagDataLoss);

  // The gap is reported exactly once: the chunk after it carries no flag.
  ASSERT_TRUE(WriteFragment(&writer, std::string(500, 'x')));
  EXPECT_EQ(Decode(ring.get(), ChunkIndex::FromIndex(2)).payload_flags, 0u);
}

// ---------------------------------------------------------------------------
// Backpressure outcomes.
// ---------------------------------------------------------------------------

TEST(SharedRingBufferWriterTest, DropPolicyReportsFull) {
  // kDrop reports a full ring buffer without blocking.
  test::SharedRingBufferForTesting ring(2, 256);
  SharedRingBufferWriter a = MakeWriter(ring.get(), kWriterA, kBuffer);
  SharedRingBufferWriter b = MakeWriter(ring.get(), kWriterB, kBuffer);

  // Two writers hold both chunks, so the ring buffer is structurally full.
  ASSERT_EQ(a.BeginFragment(1, false).result, BeginFragmentResult::kSuccess);
  ASSERT_EQ(b.BeginFragment(1, false).result, BeginFragmentResult::kSuccess);

  SharedRingBufferWriter c = MakeWriter(ring.get(), 11, kBuffer);
  EXPECT_EQ(c.BeginFragment(1, false).result, BeginFragmentResult::kFull);
  // Nothing was reserved, so a full ring buffer costs no position.
  EXPECT_EQ(ring->LoadWritePos(), 2u);
  EXPECT_EQ(c.GetStats().failed_claims, 0u);
}

TEST(SharedRingBufferWriterTest, NotifiesReaderBeforeWaiting) {
  // A full ring buffer notifies the reader before the writer waits.
  if (!SharedRingBuffer::SupportsWriterWait())
    GTEST_SKIP() << "The futex wait is not available on this platform";
  test::SharedRingBufferForTesting ring(1, 256);

  SharedRingBufferWriter first = MakeWriter(ring.get(), kWriterA, kBuffer);
  ASSERT_TRUE(WriteFragment(&first, "first"));
  ASSERT_EQ(first.FinishCurrentChunk(), EndFragmentResult::kSuccess);

  ReleasingSharedRingBufferWriterDelegate delegate(ring.get());
  SharedRingBufferWriter second(ring.get(), kWriterB, kBuffer,
                                BufferExhaustedPolicy::kStall, &delegate);
  EXPECT_EQ(second.BeginFragment(1, false).result,
            BeginFragmentResult::kSuccess);
  EXPECT_EQ(delegate.num_notifications, 1u);
}

TEST(SharedRingBufferWriterTest, StallThenDropEpisode) {
  // kStallThenDrop does not stall again until a chunk has reported the loss.
  if (!SharedRingBuffer::SupportsWriterWait())
    GTEST_SKIP() << "The futex wait is not available on this platform";
  test::SharedRingBufferForTesting ring(1, 256);

  SharedRingBufferWriter first = MakeWriter(ring.get(), kWriterA, kBuffer);
  ASSERT_TRUE(WriteFragment(&first, "first"));
  ASSERT_EQ(first.FinishCurrentChunk(), EndFragmentResult::kSuccess);

  ReleasingSharedRingBufferWriterDelegate delegate(ring.get());
  SharedRingBufferWriter second(ring.get(), kWriterB, kBuffer,
                                BufferExhaustedPolicy::kStallThenDrop,
                                &delegate);

  // RecordDataLoss() starts a drop episode. Further attempts use kDrop until
  // the loss is reported, avoiding a timeout for every dropped packet.
  second.RecordDataLoss();
  EXPECT_EQ(second.BeginFragment(1, false).result, BeginFragmentResult::kFull);
  EXPECT_EQ(delegate.num_notifications, 0u);

  uint32_t observed = ring->LoadChunkStateWord(ChunkIndex::FromIndex(0));
  ASSERT_TRUE(ring->TryReleaseCompleteChunkAsFree(0, &observed));
  ring->PublishReadPos(1);

  ASSERT_TRUE(WriteFragment(&second, "after loss"));
  EXPECT_EQ(Decode(ring.get(), ChunkIndex::FromIndex(0)).payload_flags,
            kFlagDataLoss);
  ASSERT_EQ(second.FinishCurrentChunk(), EndFragmentResult::kSuccess);

  // Publishing the loss ends the drop episode. The next exhausted acquisition
  // stalls again, which gives the delegate a chance to release the chunk.
  EXPECT_EQ(second.BeginFragment(1, false).result,
            BeginFragmentResult::kSuccess);
  EXPECT_EQ(delegate.num_notifications, 1u);
}

// A chunk pinned by a writer that stopped mid-rewrite is not the same thing as
// a full ring buffer: positions are available, but their chunks cannot be
// acquired.
TEST(SharedRingBufferWriterTest, PinnedChunks) {
  // The writer gives up after num_chunks failed claims. Nobody else reserves
  // here, so those claims land on each physical chunk once and use up the whole
  // reservation window without acquiring anything.
  test::SharedRingBufferForTesting ring(8, 256);

  // Pin every chunk in RewriteRequested, which only its owner may leave.
  // Claiming them directly leaves write_pos at zero, so there is capacity for
  // every reservation the writer below makes.
  for (uint32_t chunk_pos = 0; chunk_pos < ring->num_chunks(); ++chunk_pos) {
    const uint32_t being_written = MakeDataStateWord(
        ChunkState::kBeingWritten, ChunkFormat::kTargetBuffer, 0, 0, kWriterB);
    ASSERT_TRUE(ring->TryAcquireChunkForWriting(chunk_pos, being_written));
    const auto chunk_idx =
        ChunkIndex::FromPosition(chunk_pos, ring->num_chunks());
    uint32_t observed = being_written;
    ASSERT_TRUE(ring->TryRequestRewrite(chunk_idx, &observed));
  }
  ASSERT_EQ(ring->LoadWritePos(), 0u);

  CountingSharedRingBufferWriterDelegate delegate;
  SharedRingBufferWriter writer(ring.get(), kWriterA, kBuffer,
                                BufferExhaustedPolicy::kDrop, &delegate);
  EXPECT_EQ(writer.BeginFragment(1, false).result,
            BeginFragmentResult::kNoChunkAvailable);
  EXPECT_EQ(writer.GetStats().failed_claims, ring->num_chunks());
  EXPECT_EQ(ring->LoadWritePos(), ring->num_chunks());
  EXPECT_EQ(delegate.num_notifications, 1u);
}

// ---------------------------------------------------------------------------
// Relocation after a scrape.
// ---------------------------------------------------------------------------

TEST(SharedRingBufferWriterTest, RelocatesSuffix) {
  // A scrape moves only the unpublished suffix. The first scrape also
  // allocates the relocation scratch; the second one below reuses it.
  test::SharedRingBufferForTesting ring(4, 512);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);

  const std::vector<std::string> prefix = {"one", "", std::string(128, 'x'),
                                           "three", "four"};
  for (const auto& fragment : prefix)
    ASSERT_TRUE(WriteFragment(&writer, fragment));

  // Several published fragments, including a two-byte size and an empty
  // fragment, precede the only completed suffix this writer can produce.
  const auto range = writer.BeginFragment(6, false);
  ASSERT_EQ(range.result, BeginFragmentResult::kSuccess);
  memcpy(range.begin, "suffix", 6);
  EXPECT_EQ(MarkForRewrite(ring.get(), ChunkIndex::FromIndex(0)),
            prefix.size());
  EXPECT_EQ(Decode(ring.get(), ChunkIndex::FromIndex(0)).fragments, prefix);

  ASSERT_EQ(writer.EndFragment(6, false), EndFragmentResult::kSuccess);
  EXPECT_EQ(writer.GetStats().relocations, 1u);
  EXPECT_EQ(writer.GetStats().fragments_dropped, 0u);

  // The old chunk is acknowledged - the writer says nothing about who gets it
  // next - and only the suffix moved.
  EXPECT_EQ(ChunkStateOf(ring->LoadChunkStateWord(ChunkIndex::FromIndex(0))),
            ChunkState::kRewriteAcknowledged);
  const DecodedChunk replacement = Decode(ring.get(), ChunkIndex::FromIndex(1));
  EXPECT_EQ(replacement.state, ChunkState::kComplete);
  EXPECT_EQ(replacement.writer_id, kWriterA);
  EXPECT_EQ(replacement.target_buffer, kBuffer);
  ASSERT_EQ(replacement.fragments.size(), 1u);
  EXPECT_EQ(replacement.fragments[0], "suffix");
  // A non-empty prefix went out with the chunk's beginning, so the suffix does
  // not repeat the flags describing it.
  EXPECT_EQ(replacement.payload_flags, 0u);

  // Scrape the replacement too: the newer suffix moves on again.
  const auto again = writer.BeginFragment(5, false);
  ASSERT_EQ(again.result, BeginFragmentResult::kSuccess);
  memcpy(again.begin, "again", 5);
  EXPECT_EQ(MarkForRewrite(ring.get(), ChunkIndex::FromIndex(1)), 1u);
  ASSERT_EQ(writer.EndFragment(5, false), EndFragmentResult::kSuccess);
  EXPECT_EQ(writer.GetStats().relocations, 2u);
  EXPECT_EQ(writer.GetStats().fragments_dropped, 0u);

  EXPECT_EQ(ring->LoadChunkStateWord(ChunkIndex::FromIndex(1)),
            kRewriteAcknowledgedStateWord);
  const DecodedChunk second = Decode(ring.get(), ChunkIndex::FromIndex(2));
  EXPECT_EQ(second.state, ChunkState::kComplete);
  ASSERT_EQ(second.fragments.size(), 1u);
  EXPECT_EQ(second.fragments[0], "again");
  EXPECT_EQ(second.payload_flags, 0u);
}

TEST(SharedRingBufferWriterTest, RelocationKeepsFlags) {
  // A scrape with no published prefix carries the flags along.
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
  // relocated suffix.
  EXPECT_EQ(replacement.payload_flags,
            kFlagContinuesFromPrevChunk | kFlagDataLoss);
}

// An ended zero-byte fragment still needs a replacement and a size varint.
TEST(SharedRingBufferWriterTest, RelocatesZeroLengthFragment) {
  for (bool has_prefix : {false, true}) {
    SCOPED_TRACE(has_prefix);
    test::SharedRingBufferForTesting ring(4, 512);
    SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);
    writer.RecordDataLoss();
    if (has_prefix) {
      ASSERT_TRUE(WriteFragment(&writer, "prefix"));
    }
    ASSERT_EQ(writer.BeginFragment(0, false).result,
              BeginFragmentResult::kSuccess);
    EXPECT_EQ(MarkForRewrite(ring.get(), ChunkIndex::FromIndex(0)),
              has_prefix ? 1u : 0u);
    ASSERT_EQ(writer.EndFragment(0, false), EndFragmentResult::kSuccess);

    EXPECT_EQ(ring->LoadChunkStateWord(ChunkIndex::FromIndex(0)),
              kRewriteAcknowledgedStateWord);
    EXPECT_EQ(ring->LoadWritePos(), 2u);
    const DecodedChunk replacement =
        Decode(ring.get(), ChunkIndex::FromIndex(1));
    EXPECT_EQ(replacement.state, ChunkState::kComplete);
    ASSERT_EQ(replacement.fragments.size(), 1u);
    EXPECT_TRUE(replacement.fragments[0].empty());
    EXPECT_EQ(replacement.payload_flags, has_prefix ? 0u : kFlagDataLoss);
    EXPECT_EQ(writer.GetStats().relocations, 1u);
    EXPECT_EQ(writer.GetStats().fragments_dropped, 0u);
  }
}

// A loss flag that the reader did not take rides with the relocated suffix,
// so the writer's own data_loss_pending_ is already clear when the replacement
// is acquired. kStallThenDrop must still see the unreported loss and drop
// rather than begin another stall.
TEST(SharedRingBufferWriterTest, StallThenDropWithRelocatedLossFlag) {
  test::SharedRingBufferForTesting ring(2, 512);
  ReleasingSharedRingBufferWriterDelegate delegate(ring.get());
  SharedRingBufferWriter writer(ring.get(), kWriterA, kBuffer,
                                BufferExhaustedPolicy::kStallThenDrop,
                                &delegate);

  // The loss goes into chunk 0's flags when the writer claims it.
  writer.RecordDataLoss();
  const auto range = writer.BeginFragment(4, false);
  ASSERT_EQ(range.result, BeginFragmentResult::kSuccess);
  memcpy(range.begin, "tail", 4);

  // Occupy the ring buffer's only other chunk so no replacement can be had.
  SharedRingBufferWriter blocker = MakeWriter(ring.get(), kWriterB, kBuffer);
  ASSERT_EQ(blocker.BeginFragment(1, false).result,
            BeginFragmentResult::kSuccess);

  // The reader takes nothing, so the flag moves with the one-fragment suffix.
  EXPECT_EQ(MarkForRewrite(ring.get(), ChunkIndex::FromIndex(0)), 0u);

  // A stall would notify the delegate, which frees the acknowledged chunk and
  // lets the relocation succeed. A drop asks nobody and gives the suffix up.
  EXPECT_EQ(writer.EndFragment(4, false),
            EndFragmentResult::kRelocationDropped);
  EXPECT_EQ(delegate.num_notifications, 0u);
  EXPECT_EQ(writer.GetStats().fragments_dropped, 1u);
  EXPECT_EQ(ring->LoadChunkStateWord(ChunkIndex::FromIndex(0)),
            kRewriteAcknowledgedStateWord);

  // The loss is still unreported, so it goes out with the next chunk.
  uint32_t observed = 0;
  ASSERT_TRUE(ring->TryReleaseRewriteAcknowledgedChunkAsFree(0, &observed));
  ring->PublishReadPos(1);
  ASSERT_TRUE(WriteFragment(&writer, "after loss"));
  EXPECT_EQ(Decode(ring.get(), ChunkIndex::FromIndex(0)).payload_flags,
            kFlagDataLoss);
  EXPECT_EQ(delegate.num_notifications, 0u);
}

TEST(SharedRingBufferWriterTest, RelocationWithEmptySuffix) {
  // A scrape with nothing unpublished just acknowledges.
  test::SharedRingBufferForTesting ring(4, 512);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);

  ASSERT_TRUE(WriteFragment(&writer, "only"));
  // Take the chunk back but add nothing, then let the reader scrape it.
  ASSERT_EQ(writer.BeginFragment(1, false).result,
            BeginFragmentResult::kSuccess);
  EXPECT_EQ(MarkForRewrite(ring.get(), ChunkIndex::FromIndex(0)), 1u);

  // Releasing abandons the open fragment; there is nothing left to move.
  EXPECT_EQ(writer.FinishCurrentChunk(), EndFragmentResult::kSuccess);
  EXPECT_EQ(writer.GetStats().fragments_dropped, 0u);
  EXPECT_EQ(ChunkStateOf(ring->LoadChunkStateWord(ChunkIndex::FromIndex(0))),
            ChunkState::kRewriteAcknowledged);
  EXPECT_EQ(ChunkStateOf(ring->LoadChunkStateWord(ChunkIndex::FromIndex(1))),
            ChunkState::kFree);
}

TEST(SharedRingBufferWriterTest, EmptySuffixKeepsDataLossPending) {
  // A scrape that takes nothing, followed by nothing to relocate, leaves the
  // loss flag with the writer for its next chunk.
  test::SharedRingBufferForTesting ring(4, 512);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);
  writer.RecordDataLoss();

  ASSERT_EQ(writer.BeginFragment(4, false).result,
            BeginFragmentResult::kSuccess);
  EXPECT_EQ(MarkForRewrite(ring.get(), ChunkIndex::FromIndex(0)), 0u);

  // Releasing abandons the open fragment, so no suffix can carry the flag.
  EXPECT_EQ(writer.FinishCurrentChunk(), EndFragmentResult::kSuccess);
  EXPECT_EQ(ChunkStateOf(ring->LoadChunkStateWord(ChunkIndex::FromIndex(0))),
            ChunkState::kRewriteAcknowledged);

  ASSERT_TRUE(WriteFragment(&writer, "next"));
  EXPECT_EQ(Decode(ring.get(), ChunkIndex::FromIndex(1)).payload_flags,
            kFlagDataLoss);
}

// Acknowledging happens before the writer looks for replacement capacity. The
// other order would leave the old chunk occupied exactly when the ring buffer
// is full, so every later traversal of it would burn a position.
TEST(SharedRingBufferWriterTest, RelocationDrop) {
  for (const std::string& suffix : {std::string("lost"), std::string()}) {
    SCOPED_TRACE(suffix);
    test::SharedRingBufferForTesting ring(2, 512);
    SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);

    ASSERT_TRUE(WriteFragment(&writer, "published"));

    // Occupy the ring buffer's only other chunk so no replacement can be had.
    SharedRingBufferWriter blocker = MakeWriter(ring.get(), kWriterB, kBuffer);
    ASSERT_EQ(blocker.BeginFragment(1, false).result,
              BeginFragmentResult::kSuccess);

    const auto range = writer.BeginFragment(4, false);
    ASSERT_EQ(range.result, BeginFragmentResult::kSuccess);
    if (!suffix.empty())
      memcpy(range.begin, suffix.data(), suffix.size());
    EXPECT_EQ(MarkForRewrite(ring.get(), ChunkIndex::FromIndex(0)), 1u);

    EXPECT_EQ(writer.EndFragment(static_cast<uint32_t>(suffix.size()), false),
              EndFragmentResult::kRelocationDropped);
    EXPECT_EQ(writer.GetStats().fragments_dropped, 1u);
    EXPECT_EQ(writer.GetStats().relocations, 1u);
    // The old chunk is acknowledged and therefore reclaimable by the reader,
    // even though the data did not survive.
    EXPECT_EQ(ring->LoadChunkStateWord(ChunkIndex::FromIndex(0)),
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
    EXPECT_EQ(ring->LoadWritePos(), 2u);
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
