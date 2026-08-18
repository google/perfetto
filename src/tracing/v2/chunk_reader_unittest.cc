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

#include "src/tracing/v2/chunk_reader.h"

#include <stdint.h>
#include <string.h>

#include <memory>
#include <string>
#include <vector>

#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/tracing/v2/ring_writer.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/tracing_v2_abi.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::tracing_v2 {
namespace {

using ReadOutcome = ChunkReader::ReadOutcome;

constexpr WriterID kWriterA = 7;
constexpr WriterID kWriterB = 8;
constexpr BufferID kBuffer = 0x1234;

struct ReadChunk {
  WriterID writer_id = 0;
  BufferID target_buffer = 0;
  uint32_t payload_flags = 0;
  std::vector<std::string> fragments;
};

class RecordingDelegate : public ChunkReader::Delegate {
 public:
  void OnChunkRead(const ChunkReader::ChunkContents& contents) override {
    ReadChunk chunk;
    chunk.writer_id = contents.writer_id;
    chunk.target_buffer = contents.target_buffer;
    chunk.payload_flags = contents.payload_flags;
    for (uint32_t i = 0; i < contents.num_fragments; ++i) {
      chunk.fragments.emplace_back(
          reinterpret_cast<const char*>(contents.fragments[i].data),
          contents.fragments[i].size);
    }
    chunks.push_back(std::move(chunk));
  }

  std::vector<std::string> AllFragments() const {
    std::vector<std::string> all;
    for (const ReadChunk& chunk : chunks)
      all.insert(all.end(), chunk.fragments.begin(), chunk.fragments.end());
    return all;
  }

  std::vector<ReadChunk> chunks;
};

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

// ---------------------------------------------------------------------------
// The nominal path.
// ---------------------------------------------------------------------------

TEST(ChunkReaderTest, EmptyRingHasNoData) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  ChunkReader reader(ring.get(), &delegate);

  EXPECT_EQ(reader.ReadOne(), ReadOutcome::kNoData);
  EXPECT_EQ(reader.read_pos(), 0u);
  const ChunkReader::DrainResult result = reader.Drain(16);
  EXPECT_EQ(result.positions_resolved, 0u);
  EXPECT_FALSE(result.work_may_remain());
}

TEST(ChunkReaderTest, ConsumesACompleteChunkAndFreesItForTheNextTraversal) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  ChunkReader reader(ring.get(), &delegate);
  RingWriter writer(ring.get(), kWriterA, kBuffer,
                    BufferExhaustedPolicy::kDrop);

  ASSERT_EQ(WriteFragment(&writer, "alpha"), RingWriter::Outcome::kOk);
  ASSERT_EQ(WriteFragment(&writer, "beta"), RingWriter::Outcome::kOk);
  writer.Release();

  EXPECT_EQ(reader.ReadOne(), ReadOutcome::kEmitted);
  EXPECT_EQ(reader.read_pos(), 1u);
  ASSERT_EQ(delegate.chunks.size(), 1u);
  EXPECT_EQ(delegate.chunks[0].writer_id, kWriterA);
  EXPECT_EQ(delegate.chunks[0].target_buffer, kBuffer);
  EXPECT_EQ(delegate.chunks[0].fragments,
            (std::vector<std::string>{"alpha", "beta"}));

  // The chunk is now tagged for the traversal after the one just resolved.
  EXPECT_EQ(ring->LoadStateAcquire(0), MakeFreeForWrapWord(1));
  EXPECT_EQ(reader.ReadOne(), ReadOutcome::kNoData);
}

TEST(ChunkReaderTest, DrainPublishesReadPosOncePerPass) {
  auto ring = SharedRingBuffer::Create(8, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  ChunkReader reader(ring.get(), &delegate);

  // Four writers, one chunk each, so the pass has four positions to resolve.
  std::vector<std::unique_ptr<RingWriter>> writers;
  for (uint32_t i = 0; i < 4; ++i) {
    writers.push_back(
        std::make_unique<RingWriter>(ring.get(), static_cast<WriterID>(10 + i),
                                     kBuffer, BufferExhaustedPolicy::kDrop));
    ASSERT_EQ(WriteFragment(writers.back().get(), "x"),
              RingWriter::Outcome::kOk);
  }
  // The shared cursor has not moved yet: ReadOne() does not publish it.
  EXPECT_EQ(ring->read_pos_for_testing(), 0u);

  const ChunkReader::DrainResult result = reader.Drain(16);
  EXPECT_EQ(result.positions_resolved, 4u);
  EXPECT_EQ(result.last_outcome, ReadOutcome::kNoData);
  EXPECT_FALSE(result.work_may_remain());
  EXPECT_EQ(ring->read_pos_for_testing(), 4u);
  EXPECT_EQ(delegate.chunks.size(), 4u);
}

TEST(ChunkReaderTest, DrainStopsAtItsBudgetAndSaysWorkMayRemain) {
  auto ring = SharedRingBuffer::Create(8, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  ChunkReader reader(ring.get(), &delegate);

  std::vector<std::unique_ptr<RingWriter>> writers;
  for (uint32_t i = 0; i < 4; ++i) {
    writers.push_back(
        std::make_unique<RingWriter>(ring.get(), static_cast<WriterID>(10 + i),
                                     kBuffer, BufferExhaustedPolicy::kDrop));
    ASSERT_EQ(WriteFragment(writers.back().get(), "x"),
              RingWriter::Outcome::kOk);
  }

  const ChunkReader::DrainResult first = reader.Drain(2);
  EXPECT_EQ(first.positions_resolved, 2u);
  EXPECT_TRUE(first.work_may_remain());
  EXPECT_EQ(ring->read_pos_for_testing(), 2u);

  const ChunkReader::DrainResult second = reader.Drain(16);
  EXPECT_EQ(second.positions_resolved, 2u);
  EXPECT_FALSE(second.work_may_remain());
  EXPECT_EQ(delegate.chunks.size(), 4u);
}

// ---------------------------------------------------------------------------
// Holes.
// ---------------------------------------------------------------------------

TEST(ChunkReaderTest, UnclaimedPositionIsAHoleAndPreparesTheNextTraversal) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  ChunkReader reader(ring.get(), &delegate);

  // A writer reserved position 0 and never claimed it.
  ASSERT_EQ(ring->ReservePosition().position, 0u);

  EXPECT_EQ(reader.ReadOne(), ReadOutcome::kSkipped);
  EXPECT_EQ(reader.read_pos(), 1u);
  EXPECT_EQ(reader.num_holes(), 1u);
  EXPECT_TRUE(delegate.chunks.empty());
  EXPECT_EQ(ring->LoadStateAcquire(0), MakeFreeForWrapWord(1));
}

TEST(ChunkReaderTest, RewriteRequestedIsSkippedWithoutTouchingTheWord) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  ChunkReader reader(ring.get(), &delegate);

  // Chunk 0 is left mid-rewrite by a writer that has not come back, and a later
  // position maps onto it.
  const uint32_t acquired = MakeDataBearingWord(
      ChunkState::kAcquired, ChunkFormat::kTargetBuffer, 0, 0, kWriterB);
  ASSERT_TRUE(ring->TryClaim(0, acquired));
  uint32_t observed = acquired;
  ASSERT_TRUE(ring->TryMarkForRewrite(0, &observed));
  const uint32_t marked = ring->LoadStateAcquire(0);

  ASSERT_EQ(ring->ReservePosition().position, 0u);
  EXPECT_EQ(reader.ReadOne(), ReadOutcome::kSkipped);
  EXPECT_EQ(reader.read_pos(), 1u);
  // Only the owning writer may leave that state, so the reader left it alone.
  EXPECT_EQ(ring->LoadStateAcquire(0), marked);
  EXPECT_EQ(reader.num_holes(), 1u);
}

TEST(ChunkReaderTest, AcknowledgedIsReclaimedForTheResolvedPosition) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  ChunkReader reader(ring.get(), &delegate);

  const uint32_t acquired = MakeDataBearingWord(
      ChunkState::kAcquired, ChunkFormat::kTargetBuffer, 0, 0, kWriterB);
  ASSERT_TRUE(ring->TryClaim(0, acquired));
  uint32_t observed = acquired;
  ASSERT_TRUE(ring->TryMarkForRewrite(0, &observed));
  ASSERT_TRUE(ring->TryAcknowledge(
      0, WithState(acquired, ChunkState::kRewriteRequested)));

  ASSERT_EQ(ring->ReservePosition().position, 0u);
  EXPECT_EQ(reader.ReadOne(), ReadOutcome::kSkipped);
  EXPECT_EQ(ring->LoadStateAcquire(0), MakeFreeForWrapWord(1));
}

// ---------------------------------------------------------------------------
// Scraping a live writer.
// ---------------------------------------------------------------------------

TEST(ChunkReaderTest, TakesTheCommittedPrefixOfALiveWriter) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  ChunkReader reader(ring.get(), &delegate);
  RingWriter writer(ring.get(), kWriterA, kBuffer,
                    BufferExhaustedPolicy::kDrop);

  ASSERT_EQ(WriteFragment(&writer, "committed"), RingWriter::Outcome::kOk);
  // The writer is inside the chunk with a fragment still open.
  const RingWriter::FragmentSpan span = writer.OpenFragment(6, false);
  ASSERT_EQ(span.outcome, RingWriter::Outcome::kOk);
  memcpy(span.begin, "suffix", 6);

  EXPECT_EQ(reader.ReadOne(), ReadOutcome::kEmitted);
  EXPECT_EQ(reader.num_scrapes(), 1u);
  ASSERT_EQ(delegate.chunks.size(), 1u);
  // Only what the writer had published, and nothing of the open fragment.
  EXPECT_EQ(delegate.chunks[0].fragments,
            (std::vector<std::string>{"committed"}));
  EXPECT_EQ(StateOf(ring->LoadStateAcquire(0)), ChunkState::kRewriteRequested);

  // The writer relocates its suffix, and the reader picks it up next pass.
  ASSERT_EQ(writer.CloseFragment(6, false), RingWriter::Outcome::kOk);
  reader.Drain(8);
  EXPECT_EQ(delegate.AllFragments(),
            (std::vector<std::string>{"committed", "suffix"}));
}

TEST(ChunkReaderTest, ScrapeWithNothingCommittedStillMarksTheChunk) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  ChunkReader reader(ring.get(), &delegate);
  RingWriter writer(ring.get(), kWriterA, kBuffer,
                    BufferExhaustedPolicy::kDrop);

  ASSERT_EQ(writer.OpenFragment(4, false).outcome, RingWriter::Outcome::kOk);

  EXPECT_EQ(reader.ReadOne(), ReadOutcome::kSkipped);
  EXPECT_TRUE(delegate.chunks.empty());
  // Nothing came out, but the old owner is still stopped from publishing
  // behind the reader.
  EXPECT_EQ(StateOf(ring->LoadStateAcquire(0)), ChunkState::kRewriteRequested);
}

// The reader's mark loses to the writer's publication. The speculative copy is
// discarded and the reader redispatches on the Complete word it was handed, so
// the fragments come out exactly once and in order.
TEST(ChunkReaderTest, PublicationBeatingTheScrapeEmitsEverythingExactlyOnce) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  ChunkReader reader(ring.get(), &delegate);
  RingWriter writer(ring.get(), kWriterA, kBuffer,
                    BufferExhaustedPolicy::kDrop);

  ASSERT_EQ(WriteFragment(&writer, "first"), RingWriter::Outcome::kOk);
  const RingWriter::FragmentSpan span = writer.OpenFragment(6, false);
  ASSERT_EQ(span.outcome, RingWriter::Outcome::kOk);
  memcpy(span.begin, "second", 6);

  // Publish from inside the reader, after it has copied the prefix and just
  // before it tries to claim it. Once is enough: the redispatch lands on
  // Complete, which is not a racing state.
  bool published = false;
  reader.SetArbitrationHookForTesting([&] {
    if (published)
      return;
    published = true;
    EXPECT_EQ(writer.CloseFragment(6, false), RingWriter::Outcome::kOk);
  });

  EXPECT_EQ(reader.ReadOne(), ReadOutcome::kEmitted);
  EXPECT_EQ(delegate.AllFragments(),
            (std::vector<std::string>{"first", "second"}));
  // No relocation happened: the writer won, so there was nothing to move.
  EXPECT_EQ(writer.num_relocations(), 0u);
  EXPECT_EQ(reader.num_scrapes(), 0u);
}

TEST(ChunkReaderTest, ReuseBeatingTheReclaimIsHandledAsAScrape) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  ChunkReader reader(ring.get(), &delegate);
  RingWriter writer(ring.get(), kWriterA, kBuffer,
                    BufferExhaustedPolicy::kDrop);

  ASSERT_EQ(WriteFragment(&writer, "first"), RingWriter::Outcome::kOk);

  // The writer takes the Complete chunk back just as the reader is about to
  // reclaim it. The reader's reclaim fails, it redispatches on Acquired, and
  // takes the committed prefix instead.
  bool reused = false;
  reader.SetArbitrationHookForTesting([&] {
    if (reused)
      return;
    reused = true;
    ASSERT_EQ(writer.OpenFragment(3, false).outcome, RingWriter::Outcome::kOk);
  });

  EXPECT_EQ(reader.ReadOne(), ReadOutcome::kEmitted);
  EXPECT_EQ(delegate.AllFragments(), (std::vector<std::string>{"first"}));
  EXPECT_EQ(reader.num_scrapes(), 1u);
  EXPECT_EQ(StateOf(ring->LoadStateAcquire(0)), ChunkState::kRewriteRequested);
}

// A writer that keeps winning is a writer that is making progress. The reader
// gives up on this pass rather than spinning, and leaves read_pos alone so the
// same position is retried later.
TEST(ChunkReaderTest, ContentionBudgetExhaustionReturnsRetryLater) {
  auto ring = SharedRingBuffer::Create(4, 32768);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  ChunkReader reader(ring.get(), &delegate);
  RingWriter writer(ring.get(), kWriterA, kBuffer,
                    BufferExhaustedPolicy::kDrop);

  ASSERT_EQ(WriteFragment(&writer, "seed"), RingWriter::Outcome::kOk);
  // Alternate reuse and publish under the reader's feet, so every one of its
  // compare-and-swaps loses.
  bool acquired = false;
  reader.SetArbitrationHookForTesting([&] {
    if (acquired) {
      EXPECT_EQ(writer.CloseFragment(1, false), RingWriter::Outcome::kOk);
    } else {
      ASSERT_EQ(writer.OpenFragment(1, false).outcome,
                RingWriter::Outcome::kOk);
    }
    acquired = !acquired;
  });

  EXPECT_EQ(reader.ReadOne(), ReadOutcome::kRetryLater);
  EXPECT_EQ(reader.read_pos(), 0u);
  EXPECT_TRUE(delegate.chunks.empty());

  // With the interference removed the same position resolves normally.
  reader.SetArbitrationHookForTesting(nullptr);
  writer.Release();
  EXPECT_EQ(reader.ReadOne(), ReadOutcome::kEmitted);
  EXPECT_EQ(reader.read_pos(), 1u);
}

// ---------------------------------------------------------------------------
// Untrusted input.
// ---------------------------------------------------------------------------

TEST(ChunkReaderTest, UnknownFormatDropsThePayloadButReleasesTheChunk) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  ChunkReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->ReservePosition().position, 0u);
  ring->SetStateWordForTesting(
      0, MakeDataBearingWord(ChunkState::kComplete,
                             ChunkFormat::kReservedRouting, 0, 3, kWriterB));

  EXPECT_EQ(reader.ReadOne(), ReadOutcome::kSkipped);
  EXPECT_EQ(reader.num_unknown_format_chunks(), 1u);
  EXPECT_TRUE(delegate.chunks.empty());
  // Losing bytes it cannot parse is fine; getting the ring stuck is not.
  EXPECT_EQ(ring->LoadStateAcquire(0), MakeFreeForWrapWord(1));
  EXPECT_EQ(reader.read_pos(), 1u);
}

TEST(ChunkReaderTest, DirectoryLargerThanTheChunkIsRejectedBoundedly) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  ChunkReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->ReservePosition().position, 0u);
  // 255 one-byte entries against a 250-byte payload area: the directory alone
  // does not fit.
  ring->SetStateWordForTesting(
      0, MakeDataBearingWord(ChunkState::kComplete, ChunkFormat::kTargetBuffer,
                             0, 255, kWriterB));

  EXPECT_EQ(reader.ReadOne(), ReadOutcome::kSkipped);
  EXPECT_EQ(reader.num_malformed_chunks(), 1u);
  EXPECT_TRUE(delegate.chunks.empty());
  EXPECT_EQ(ring->LoadStateAcquire(0), MakeFreeForWrapWord(1));
}

TEST(ChunkReaderTest, CumulativeFragmentSizesBeyondTheChunkAreRejected) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  ChunkReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->ReservePosition().position, 0u);
  // Two fragments claiming 400 bytes each in a chunk that has 506 bytes of
  // payload area, of which four go to the directory.
  uint8_t* chunk = ring->chunk_at(0);
  const uint32_t width = ring->fragment_size_width();
  StoreFragmentSize(chunk + FragmentSizeEntryOffset(512, width, 0), width, 400);
  StoreFragmentSize(chunk + FragmentSizeEntryOffset(512, width, 1), width, 400);
  ring->SetStateWordForTesting(
      0, MakeDataBearingWord(ChunkState::kComplete, ChunkFormat::kTargetBuffer,
                             0, 2, kWriterB));

  EXPECT_EQ(reader.ReadOne(), ReadOutcome::kSkipped);
  EXPECT_EQ(reader.num_malformed_chunks(), 1u);
  EXPECT_TRUE(delegate.chunks.empty());
  EXPECT_EQ(ring->LoadStateAcquire(0), MakeFreeForWrapWord(1));
}

// A count larger than the writer actually wrote is a producer claim like any
// other. It cannot make the reader leave the chunk or spin; it just decodes the
// stale bytes underneath as extra fragments.
TEST(ChunkReaderTest, InflatedFragmentCountStaysInsideTheChunk) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  ChunkReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->ReservePosition().position, 0u);
  ring->SetStateWordForTesting(
      0, MakeDataBearingWord(ChunkState::kComplete, ChunkFormat::kTargetBuffer,
                             0, 8, kWriterB));

  EXPECT_EQ(reader.ReadOne(), ReadOutcome::kEmitted);
  // Over a zero-filled mapping the inflated count decodes as extra zero-length
  // fragments. The checks make the walk memory-safe; they do not pretend to
  // make its contents true.
  ASSERT_EQ(delegate.chunks.size(), 1u);
  EXPECT_EQ(delegate.chunks[0].fragments.size(), 8u);
  for (const std::string& fragment : delegate.chunks[0].fragments)
    EXPECT_TRUE(fragment.empty());
  EXPECT_EQ(reader.num_malformed_chunks(), 0u);
  EXPECT_EQ(ring->LoadStateAcquire(0), MakeFreeForWrapWord(1));
  EXPECT_EQ(reader.read_pos(), 1u);
}

TEST(ChunkReaderTest, MalformedAcquiredChunkIsStillRewriteRequested) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  ChunkReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->ReservePosition().position, 0u);
  ring->SetStateWordForTesting(
      0, MakeDataBearingWord(ChunkState::kAcquired, ChunkFormat::kTargetBuffer,
                             0, 255, kWriterB));

  EXPECT_EQ(reader.ReadOne(), ReadOutcome::kSkipped);
  EXPECT_EQ(reader.num_malformed_chunks(), 1u);
  // Malformed bytes may be dropped. What must not happen is leaving an
  // Acquired owner able to publish behind the reader.
  EXPECT_EQ(StateOf(ring->LoadStateAcquire(0)), ChunkState::kRewriteRequested);
}

TEST(ChunkReaderTest, ReservedStateStopsTheRingWithoutChangingAnything) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  ChunkReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->ReservePosition().position, 0u);
  const uint32_t reserved_word = 0xa0000000u;
  ring->SetStateWordForTesting(0, reserved_word);

  EXPECT_EQ(reader.ReadOne(), ReadOutcome::kProtocolError);
  EXPECT_TRUE(reader.stopped());
  EXPECT_EQ(reader.read_pos(), 0u);
  EXPECT_EQ(ring->LoadStateAcquire(0), reserved_word);

  // The ring is never read again, and a drain over it changes nothing.
  const ChunkReader::DrainResult result = reader.Drain(8);
  EXPECT_EQ(result.positions_resolved, 0u);
  EXPECT_EQ(result.last_outcome, ReadOutcome::kProtocolError);
  EXPECT_EQ(ring->read_pos_for_testing(), 0u);
}

TEST(ChunkReaderTest, FreeWordFromAnotherTraversalStopsTheRing) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  ChunkReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->ReservePosition().position, 0u);
  // Position 0 expects FreeForWrap(0). Only the reader writes a free word, and
  // it derives it from the position it is resolving, so no legal execution puts
  // this here.
  const uint32_t wrong_wrap = MakeFreeForWrapWord(3);
  ring->SetStateWordForTesting(0, wrong_wrap);

  EXPECT_EQ(reader.ReadOne(), ReadOutcome::kProtocolError);
  EXPECT_TRUE(reader.stopped());
  EXPECT_EQ(reader.read_pos(), 0u);
  EXPECT_EQ(ring->LoadStateAcquire(0), wrong_wrap);
}

// A writer reserves a position only after seeing fewer than num_chunks
// outstanding, so a cursor further ahead than that cannot come from a legal
// producer. Once the mapping is writable by another process, believing it would
// mean resolving positions that were never reserved, one drain pass after
// another, for as long as the producer keeps the cursor there.
TEST(ChunkReaderTest, ImpossibleWritePosDistanceStopsTheRing) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  ChunkReader reader(ring.get(), &delegate);

  ring->SetWritePosForTesting(5);  // Four chunks, five outstanding positions.

  EXPECT_EQ(reader.ReadOne(), ReadOutcome::kProtocolError);
  EXPECT_TRUE(reader.stopped());
  EXPECT_EQ(reader.read_pos(), 0u);
  EXPECT_EQ(ring->read_pos_for_testing(), 0u);
  EXPECT_TRUE(delegate.chunks.empty());
}

// The boundary itself is legal and must still be drained: num_chunks
// outstanding positions is exactly a full ring.
TEST(ChunkReaderTest, AFullRingIsNotAnImpossibleDistance) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  ChunkReader reader(ring.get(), &delegate);

  RingWriter writer(ring.get(), kWriterA, kBuffer,
                    BufferExhaustedPolicy::kDrop);
  for (uint32_t i = 0; i < 4; ++i) {
    ASSERT_EQ(WriteFragment(&writer, "x"), RingWriter::Outcome::kOk);
    writer.Release();
  }
  ASSERT_EQ(ring->LoadWritePosRelaxed(), 4u);

  const ChunkReader::DrainResult result = reader.Drain(8);
  EXPECT_FALSE(reader.stopped());
  EXPECT_EQ(result.positions_resolved, 4u);
  EXPECT_EQ(delegate.chunks.size(), 4u);
}

// ---------------------------------------------------------------------------
// Flags and routing survive the trip.
// ---------------------------------------------------------------------------

TEST(ChunkReaderTest, ContinuationFlagsAndTargetBufferReachTheDelegate) {
  auto ring = SharedRingBuffer::Create(8, 512);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  ChunkReader reader(ring.get(), &delegate);

  RingWriter first(ring.get(), kWriterA, 11, BufferExhaustedPolicy::kDrop);
  first.RecordDataLoss();
  ASSERT_EQ(WriteFragment(&first, "head", /*continues_from_prev=*/false,
                          /*continues_on_next=*/true),
            RingWriter::Outcome::kOk);
  ASSERT_EQ(WriteFragment(&first, "tail", /*continues_from_prev=*/true,
                          /*continues_on_next=*/false),
            RingWriter::Outcome::kOk);
  first.Release();

  RingWriter second(ring.get(), kWriterB, 22, BufferExhaustedPolicy::kDrop);
  ASSERT_EQ(WriteFragment(&second, "other"), RingWriter::Outcome::kOk);
  second.Release();

  reader.Drain(16);
  ASSERT_EQ(delegate.chunks.size(), 3u);

  EXPECT_EQ(delegate.chunks[0].target_buffer, 11);
  EXPECT_EQ(delegate.chunks[0].payload_flags,
            kFlagDataLoss | kFlagContinuesOnNextChunk);
  EXPECT_EQ(delegate.chunks[1].target_buffer, 11);
  EXPECT_EQ(delegate.chunks[1].payload_flags, kFlagContinuesFromPrevChunk);
  EXPECT_EQ(delegate.chunks[2].target_buffer, 22);
  EXPECT_EQ(delegate.chunks[2].writer_id, kWriterB);
  EXPECT_EQ(delegate.chunks[2].payload_flags, 0u);
}

TEST(ChunkReaderTest, FragmentsComeOutInReservationOrderAcrossWriters) {
  auto ring = SharedRingBuffer::Create(8, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  ChunkReader reader(ring.get(), &delegate);

  // Interleave two writers so their chunks land at alternating positions.
  RingWriter a(ring.get(), kWriterA, kBuffer, BufferExhaustedPolicy::kDrop);
  RingWriter b(ring.get(), kWriterB, kBuffer, BufferExhaustedPolicy::kDrop);
  std::vector<std::string> expected;
  for (uint32_t i = 0; i < 4; ++i) {
    // Each fragment fills its chunk, so every write consumes a new position.
    const std::string from_a = "a" + std::to_string(i) + std::string(240, '.');
    const std::string from_b = "b" + std::to_string(i) + std::string(240, '.');
    ASSERT_EQ(WriteFragment(&a, from_a), RingWriter::Outcome::kOk);
    ASSERT_EQ(WriteFragment(&b, from_b), RingWriter::Outcome::kOk);
    expected.push_back(from_a);
    expected.push_back(from_b);
    reader.Drain(16);
  }
  a.Release();
  b.Release();
  reader.Drain(16);

  EXPECT_EQ(delegate.AllFragments(), expected);
}

}  // namespace
}  // namespace perfetto::tracing_v2
