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

#include "src/tracing/v2/shared_ring_buffer_reader.h"

#include <stdint.h>
#include <string.h>

#include <memory>
#include <string>
#include <vector>

#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"
#include "src/tracing/v2/shared_ring_buffer_test_utils.h"
#include "src/tracing/v2/shared_ring_buffer_writer.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::tracing_v2 {
namespace {

using Internals = test::SharedRingBufferInternalsForTest;
using BeginFragmentResult = SharedRingBufferWriter::BeginFragmentResult;
using EndFragmentResult = SharedRingBufferWriter::EndFragmentResult;
using EndFragmentResult = SharedRingBufferWriter::EndFragmentResult;
using ConsumeResult = SharedRingBufferReader::ConsumeResult;
using test::GetNoopWriterDelegate;
using test::MakeWriter;
using test::WriteFragment;

constexpr WriterID kWriterA = 7;
constexpr WriterID kWriterB = 8;
constexpr BufferID kBuffer = 0x1234;

struct ReadChunk {
  WriterID writer_id = 0;
  BufferID target_buffer = 0;
  uint32_t payload_flags = 0;
  std::vector<std::string> fragments;
};

class RecordingDelegate : public SharedRingBufferReader::Delegate {
 public:
  void OnChunkRead(
      const SharedRingBufferReader::ChunkContents& contents) override {
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

  void OnDataLoss(WriterID writer_id) override {
    writers_with_data_loss.push_back(writer_id);
  }

  std::vector<std::string> AllFragments() const {
    std::vector<std::string> all;
    for (const ReadChunk& chunk : chunks)
      all.insert(all.end(), chunk.fragments.begin(), chunk.fragments.end());
    return all;
  }

  std::vector<ReadChunk> chunks;
  std::vector<WriterID> writers_with_data_loss;
};

// ---------------------------------------------------------------------------
// The nominal path.
// ---------------------------------------------------------------------------

TEST(SharedRingBufferReaderTest, EmptyRing) {
  test::SharedRingBufferForTesting ring(4, 256);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  EXPECT_EQ(Internals::ConsumeNextPosition(&reader), ConsumeResult::kNoData);
  EXPECT_EQ(reader.read_pos(), 0u);
  const SharedRingBufferReader::DrainResult result = reader.Drain(16);
  EXPECT_EQ(result.positions_consumed, 0u);
  EXPECT_FALSE(result.needs_another_drain());
}

TEST(SharedRingBufferReaderTest, ConsumesCompleteChunk) {
  // A Complete chunk is consumed and freed for the next traversal.
  test::SharedRingBufferForTesting ring(4, 512);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);

  ASSERT_TRUE(WriteFragment(&writer, "alpha"));
  ASSERT_TRUE(WriteFragment(&writer, "beta"));
  writer.FinishCurrentChunk();

  EXPECT_EQ(Internals::ConsumeNextPosition(&reader), ConsumeResult::kChunkRead);
  EXPECT_EQ(reader.read_pos(), 1u);
  ASSERT_EQ(delegate.chunks.size(), 1u);
  EXPECT_EQ(delegate.chunks[0].writer_id, kWriterA);
  EXPECT_EQ(delegate.chunks[0].target_buffer, kBuffer);
  EXPECT_EQ(delegate.chunks[0].fragments,
            (std::vector<std::string>{"alpha", "beta"}));

  // The chunk is now Free with the wrap count of its next position.
  EXPECT_EQ(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0)),
            MakeFreeStateWord(1));
  EXPECT_EQ(Internals::ConsumeNextPosition(&reader), ConsumeResult::kNoData);
}

TEST(SharedRingBufferReaderTest, DrainRetriesWriterClaim) {
  test::SharedRingBufferForTesting ring(4, 256);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);
  ASSERT_EQ(ring->TryReserveWritePos().result,
            SharedRingBuffer::ReserveResult::kReserved);

  uint32_t attempts = 0;
  Internals::SetBeforeStateTransitionCallback(&reader, [&] {
    EXPECT_EQ(reader.read_pos(), 0u);
    if (++attempts == 1) {
      // Claim after the reader observed Free. Its wrap-advance CAS must fail.
      ASSERT_TRUE(ring->TryAcquireChunkForWriting(
          0, MakeDataStateWord(ChunkState::kBeingWritten,
                               ChunkFormat::kTargetBuffer, 0, 0, kWriterA)));
    }
  });

  const auto result = reader.Drain(1);
  EXPECT_EQ(attempts, 2u);
  EXPECT_EQ(result.positions_consumed, 1u);
  EXPECT_EQ(result.last_result, ConsumeResult::kPositionSkipped);
  EXPECT_EQ(Internals::GetReadPos(ring.get()), 1u);
  EXPECT_EQ(reader.GetStats().rewrite_requests, 1u);
  EXPECT_TRUE(delegate.chunks.empty());
  EXPECT_TRUE(delegate.writers_with_data_loss.empty());
}

TEST(SharedRingBufferReaderTest, DrainRetriesWriterPublication) {
  for (bool abandon_fragment : {false, true}) {
    for (bool report_loss : {false, true}) {
      SCOPED_TRACE(abandon_fragment);
      SCOPED_TRACE(report_loss);
      test::SharedRingBufferForTesting ring(4, 256);
      RecordingDelegate delegate;
      SharedRingBufferReader reader(ring.get(), &delegate);
      SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);
      ASSERT_TRUE(WriteFragment(&writer, "before"));
      if (report_loss)
        writer.RecordDataLoss();
      const auto range = writer.BeginFragment(5, false);
      ASSERT_EQ(range.result, BeginFragmentResult::kSuccess);
      memcpy(range.begin, "after", 5);

      uint32_t attempts = 0;
      Internals::SetBeforeStateTransitionCallback(&reader, [&] {
        EXPECT_EQ(reader.read_pos(), 0u);
        EXPECT_EQ(Internals::GetReadPos(ring.get()), 0u);
        EXPECT_TRUE(delegate.chunks.empty());
        EXPECT_TRUE(delegate.writers_with_data_loss.empty());
        if (++attempts != 1)
          return;
        // Publish after the reader copied "before" from BeingWritten.
        const auto result = abandon_fragment ? writer.FinishCurrentChunk()
                                             : writer.EndFragment(5, false);
        ASSERT_EQ(result, EndFragmentResult::kSuccess);
      });

      const auto result = reader.Drain(1);
      EXPECT_EQ(attempts, 2u);
      EXPECT_EQ(result.positions_consumed, 1u);
      EXPECT_EQ(Internals::GetReadPos(ring.get()), 1u);
      if (report_loss) {
        EXPECT_EQ(result.last_result, ConsumeResult::kPositionSkipped);
        EXPECT_TRUE(delegate.chunks.empty());
        EXPECT_EQ(delegate.writers_with_data_loss,
                  std::vector<WriterID>{kWriterA});
      } else {
        EXPECT_EQ(result.last_result, ConsumeResult::kChunkRead);
        ASSERT_EQ(delegate.chunks.size(), 1u);
        const std::vector<std::string> expected =
            abandon_fragment ? std::vector<std::string>{"before"}
                             : std::vector<std::string>{"before", "after"};
        EXPECT_EQ(delegate.AllFragments(), expected);
        EXPECT_TRUE(delegate.writers_with_data_loss.empty());
      }
    }
  }
}

TEST(SharedRingBufferReaderTest, DrainRetriesWriterReuse) {
  test::SharedRingBufferForTesting ring(4, 256);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);
  ASSERT_TRUE(WriteFragment(&writer, "before"));

  uint32_t attempts = 0;
  Internals::SetBeforeStateTransitionCallback(&reader, [&] {
    EXPECT_EQ(reader.read_pos(), 0u);
    EXPECT_TRUE(delegate.chunks.empty());
    if (++attempts == 1) {
      // Reuse after the reader copied Complete. Its reclaim CAS must fail.
      const auto range = writer.BeginFragment(5, false);
      ASSERT_EQ(range.result, BeginFragmentResult::kSuccess);
      memcpy(range.begin, "after", 5);
    }
  });

  const auto result = reader.Drain(1);
  EXPECT_EQ(attempts, 2u);
  EXPECT_EQ(result.positions_consumed, 1u);
  EXPECT_EQ(result.last_result, ConsumeResult::kChunkRead);
  ASSERT_EQ(delegate.chunks.size(), 1u);
  EXPECT_EQ(delegate.AllFragments(), std::vector<std::string>{"before"});
  EXPECT_EQ(reader.GetStats().rewrite_requests, 1u);

  Internals::SetBeforeStateTransitionCallback(&reader, {});
  ASSERT_EQ(writer.EndFragment(5, false), EndFragmentResult::kSuccess);
  EXPECT_EQ(writer.GetStats().relocations, 1u);
  reader.Drain(4);
  EXPECT_EQ(delegate.AllFragments(),
            (std::vector<std::string>{"before", "after"}));
}

TEST(SharedRingBufferReaderTest, DrainRetriesUntilFragmentLimit) {
  test::SharedRingBufferForTesting ring(4, 512);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);
  ASSERT_EQ(writer.BeginFragment(0, false).result,
            BeginFragmentResult::kSuccess);

  uint32_t attempts = 0;
  uint32_t fragments_published = 0;
  bool fragment_open = true;
  Internals::SetBeforeStateTransitionCallback(&reader, [&] {
    ++attempts;
    EXPECT_EQ(reader.read_pos(), 0u);
    EXPECT_TRUE(delegate.chunks.empty());
    if (fragments_published == kMaxFragmentsPerChunk)
      return;
    // Alternate publication and reuse so every CAS loses until the chunk
    // reaches the fragment limit. Zero-byte fragments still use the count.
    if (fragment_open) {
      ASSERT_EQ(writer.EndFragment(0, false), EndFragmentResult::kSuccess);
      ++fragments_published;
    } else {
      ASSERT_EQ(writer.BeginFragment(0, false).result,
                BeginFragmentResult::kSuccess);
    }
    fragment_open = !fragment_open;
  });

  const auto result = reader.Drain(1);
  EXPECT_EQ(attempts, 2 * kMaxFragmentsPerChunk);
  EXPECT_EQ(result.positions_consumed, 1u);
  EXPECT_EQ(result.last_result, ConsumeResult::kChunkRead);
  ASSERT_EQ(delegate.chunks.size(), 1u);
  EXPECT_EQ(delegate.AllFragments(),
            std::vector<std::string>(kMaxFragmentsPerChunk));
  EXPECT_FALSE(reader.has_protocol_error());
}

TEST(SharedRingBufferReaderTest, RetryLimitPublishesEarlierProgress) {
  test::SharedRingBufferForTesting ring(4, 256);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);
  ASSERT_TRUE(WriteFragment(&writer, "first"));
  writer.FinishCurrentChunk();
  writer.RecordDataLoss();
  ASSERT_TRUE(WriteFragment(&writer, "discarded"));
  writer.FinishCurrentChunk();

  uint32_t failed_attempts = 0;
  Internals::SetBeforeStateTransitionCallback(&reader, [&] {
    if (reader.read_pos() == 0)
      return;
    EXPECT_EQ(reader.read_pos(), 1u);
    EXPECT_EQ(Internals::GetReadPos(ring.get()), 0u);
    EXPECT_TRUE(delegate.writers_with_data_loss.empty());
    // Exercise the scheduling limit through the low-level API. Keep changing
    // state at the same fragment count, beyond what the writer class does.
    const ChunkIndex chunk_idx = ChunkIndex::FromIndex(1);
    uint32_t word = ring->LoadChunkStateWordAcquire(chunk_idx);
    if (ChunkStateOf(word) == ChunkState::kComplete) {
      ASSERT_TRUE(ring->TryReacquireChunkForWriting(chunk_idx, word));
    } else {
      ASSERT_EQ(ChunkStateOf(word), ChunkState::kBeingWritten);
      ASSERT_TRUE(ring->TryReleaseChunkAsComplete(
          chunk_idx, ReplaceChunkState(word, ChunkState::kComplete), &word));
    }
    ++failed_attempts;
  });

  const auto result = reader.Drain(2);
  EXPECT_EQ(failed_attempts, 2 * kMaxFragmentsPerChunk + 2);
  EXPECT_EQ(result.last_result, ConsumeResult::kRetryImmediately);
  EXPECT_TRUE(result.needs_another_drain());
  EXPECT_EQ(result.positions_consumed, 1u);
  EXPECT_EQ(reader.read_pos(), 1u);
  EXPECT_EQ(Internals::GetReadPos(ring.get()), 1u);
  EXPECT_EQ(delegate.AllFragments(), std::vector<std::string>{"first"});
  EXPECT_TRUE(delegate.writers_with_data_loss.empty());
  EXPECT_FALSE(reader.has_protocol_error());

  Internals::SetBeforeStateTransitionCallback(&reader, {});
  const auto next = reader.Drain(1);
  EXPECT_EQ(next.positions_consumed, 1u);
  EXPECT_EQ(next.last_result, ConsumeResult::kPositionSkipped);
  EXPECT_EQ(Internals::GetReadPos(ring.get()), 2u);
  EXPECT_EQ(delegate.writers_with_data_loss, std::vector<WriterID>{kWriterA});
  EXPECT_EQ(delegate.AllFragments(), std::vector<std::string>{"first"});
}

TEST(SharedRingBufferReaderTest, DrainPublishesOnce) {
  test::SharedRingBufferForTesting ring(8, 256);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  // Four writers, one chunk each, so the pass has four positions to consume.
  std::vector<std::unique_ptr<SharedRingBufferWriter>> writers;
  for (uint32_t i = 0; i < 4; ++i) {
    writers.push_back(std::make_unique<SharedRingBufferWriter>(
        ring.get(), static_cast<WriterID>(10 + i), kBuffer,
        BufferExhaustedPolicy::kDrop, GetNoopWriterDelegate()));
    ASSERT_TRUE(WriteFragment(writers.back().get(), "x"));
  }
  // The shared read_pos has not moved yet: ConsumeNextPosition() does not
  // publish it.
  EXPECT_EQ(Internals::GetReadPos(ring.get()), 0u);

  const SharedRingBufferReader::DrainResult result = reader.Drain(16);
  EXPECT_EQ(result.positions_consumed, 4u);
  EXPECT_EQ(result.last_result, ConsumeResult::kNoData);
  EXPECT_FALSE(result.needs_another_drain());
  EXPECT_EQ(Internals::GetReadPos(ring.get()), 4u);
  EXPECT_EQ(delegate.chunks.size(), 4u);
}

TEST(SharedRingBufferReaderTest, DrainBudget) {
  // Drain stops at its budget and reports that work may remain.
  test::SharedRingBufferForTesting ring(8, 256);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  std::vector<std::unique_ptr<SharedRingBufferWriter>> writers;
  for (uint32_t i = 0; i < 4; ++i) {
    writers.push_back(std::make_unique<SharedRingBufferWriter>(
        ring.get(), static_cast<WriterID>(10 + i), kBuffer,
        BufferExhaustedPolicy::kDrop, GetNoopWriterDelegate()));
    ASSERT_TRUE(WriteFragment(writers.back().get(), "x"));
  }

  const SharedRingBufferReader::DrainResult first = reader.Drain(2);
  EXPECT_EQ(first.positions_consumed, 2u);
  EXPECT_TRUE(first.needs_another_drain());
  EXPECT_EQ(Internals::GetReadPos(ring.get()), 2u);

  const SharedRingBufferReader::DrainResult second = reader.Drain(16);
  EXPECT_EQ(second.positions_consumed, 2u);
  EXPECT_FALSE(second.needs_another_drain());
  EXPECT_EQ(delegate.chunks.size(), 4u);
}

TEST(SharedRingBufferReaderTest, DrainBudgetIncludesUnclaimedReservations) {
  test::SharedRingBufferForTesting ring(4, 256);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);

  // The first reservation delivers nothing. The second holds a fragment.
  ASSERT_EQ(ring->TryReserveWritePos().chunk_pos, 0u);
  ASSERT_TRUE(WriteFragment(&writer, "published"));
  ASSERT_EQ(writer.FinishCurrentChunk(), EndFragmentResult::kSuccess);
  ASSERT_EQ(ring->TryReserveWritePos().chunk_pos, 2u);

  const auto first = reader.Drain(2);
  EXPECT_EQ(first.positions_consumed, 2u);
  EXPECT_TRUE(first.needs_another_drain());
  EXPECT_EQ(Internals::GetReadPos(ring.get()), 2u);
  EXPECT_EQ(delegate.AllFragments(), (std::vector<std::string>{"published"}));
  EXPECT_EQ(reader.GetStats().positions_skipped, 1u);

  // The third reservation remains for the next pass, even though it is empty.
  const auto second = reader.Drain(2);
  EXPECT_EQ(second.positions_consumed, 1u);
  EXPECT_FALSE(second.needs_another_drain());
  EXPECT_EQ(Internals::GetReadPos(ring.get()), 3u);
  EXPECT_EQ(delegate.chunks.size(), 1u);
  EXPECT_EQ(reader.GetStats().positions_skipped, 2u);
}

TEST(SharedRingBufferReaderTest, DrainAcrossPositionRollover) {
  test::SharedRingBufferForTesting ring(4, 256);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);
  constexpr uint32_t kStartPos = UINT32_MAX - 1;
  Internals::SetPositions(ring.get(), kStartPos);
  Internals::SetReaderPos(&reader, kStartPos);

  // Each writer reserves a fresh position: UINT32_MAX - 1, UINT32_MAX, zero.
  const std::vector<std::string> fragments = {"before", "at max", "after"};
  for (const auto& fragment : fragments) {
    SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);
    ASSERT_TRUE(WriteFragment(&writer, fragment));
    ASSERT_EQ(writer.FinishCurrentChunk(), EndFragmentResult::kSuccess);
  }

  const auto result = reader.Drain(4);
  EXPECT_EQ(result.positions_consumed, 3u);
  EXPECT_EQ(result.last_result, ConsumeResult::kNoData);
  EXPECT_FALSE(result.needs_another_drain());
  EXPECT_EQ(reader.read_pos(), 1u);
  EXPECT_EQ(Internals::GetReadPos(ring.get()), 1u);
  EXPECT_EQ(delegate.AllFragments(), fragments);

  EXPECT_EQ(reader.Drain(4).positions_consumed, 0u);
  EXPECT_EQ(delegate.chunks.size(), 3u);
}

// ---------------------------------------------------------------------------
// Unclaimed reservations.
// ---------------------------------------------------------------------------

TEST(SharedRingBufferReaderTest, UnclaimedPosition) {
  // The reader must advance the chunk's wrap count before advancing read_pos
  // past this unclaimed reservation. No fragments are delivered.
  test::SharedRingBufferForTesting ring(4, 256);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  // A writer reserved position 0 and never claimed it.
  ASSERT_EQ(ring->TryReserveWritePos().chunk_pos, 0u);

  EXPECT_EQ(Internals::ConsumeNextPosition(&reader),
            ConsumeResult::kPositionSkipped);
  EXPECT_EQ(reader.read_pos(), 1u);
  EXPECT_EQ(reader.GetStats().positions_skipped, 1u);
  EXPECT_TRUE(delegate.chunks.empty());
  EXPECT_EQ(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0)),
            MakeFreeStateWord(1));
}

TEST(SharedRingBufferReaderTest, RewriteRequestedSkipped) {
  // RewriteRequested is skipped without touching the word.
  test::SharedRingBufferForTesting ring(4, 256);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  // Chunk 0 is left mid-rewrite by a writer that has not come back, and a later
  // position maps onto it.
  const uint32_t being_written = MakeDataStateWord(
      ChunkState::kBeingWritten, ChunkFormat::kTargetBuffer, 0, 0, kWriterB);
  ASSERT_TRUE(ring->TryAcquireChunkForWriting(0, being_written));
  uint32_t observed = being_written;
  ASSERT_TRUE(ring->TryRequestRewrite(ChunkIndex::FromIndex(0), &observed));
  const uint32_t marked =
      ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0));

  ASSERT_EQ(ring->TryReserveWritePos().chunk_pos, 0u);
  EXPECT_EQ(Internals::ConsumeNextPosition(&reader),
            ConsumeResult::kPositionSkipped);
  EXPECT_EQ(reader.read_pos(), 1u);
  // Only the owning writer may leave that state, so the reader left it alone.
  EXPECT_EQ(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0)), marked);
  EXPECT_EQ(reader.GetStats().positions_skipped, 1u);
}

TEST(SharedRingBufferReaderTest, RewriteAcknowledgedReclaimed) {
  test::SharedRingBufferForTesting ring(4, 256);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  const uint32_t being_written = MakeDataStateWord(
      ChunkState::kBeingWritten, ChunkFormat::kTargetBuffer, 0, 0, kWriterB);
  ASSERT_TRUE(ring->TryAcquireChunkForWriting(0, being_written));
  uint32_t observed = being_written;
  ASSERT_TRUE(ring->TryRequestRewrite(ChunkIndex::FromIndex(0), &observed));
  ASSERT_TRUE(ring->TryAcknowledgeRewrite(
      ChunkIndex::FromIndex(0),
      ReplaceChunkState(being_written, ChunkState::kRewriteRequested)));

  ASSERT_EQ(ring->TryReserveWritePos().chunk_pos, 0u);
  EXPECT_EQ(Internals::ConsumeNextPosition(&reader),
            ConsumeResult::kPositionSkipped);
  EXPECT_EQ(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0)),
            MakeFreeStateWord(1));
}

// ---------------------------------------------------------------------------
// Scraping a live writer.
// ---------------------------------------------------------------------------

TEST(SharedRingBufferReaderTest, ReadsPublishedFragmentsWhileWriterAppends) {
  // The reader takes the published fragments of a live writer.
  test::SharedRingBufferForTesting ring(4, 512);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);

  ASSERT_TRUE(WriteFragment(&writer, "committed"));
  // The writer is inside the chunk with a fragment still open.
  const auto range = writer.BeginFragment(6, false);
  ASSERT_EQ(range.result, BeginFragmentResult::kSuccess);
  memcpy(range.begin, "suffix", 6);

  EXPECT_EQ(Internals::ConsumeNextPosition(&reader), ConsumeResult::kChunkRead);
  EXPECT_EQ(reader.GetStats().rewrite_requests, 1u);
  ASSERT_EQ(delegate.chunks.size(), 1u);
  // Only what the writer had published, and nothing of the open fragment.
  EXPECT_EQ(delegate.chunks[0].fragments,
            (std::vector<std::string>{"committed"}));
  EXPECT_EQ(
      ChunkStateOf(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0))),
      ChunkState::kRewriteRequested);

  // The writer relocates its unpublished fragment, and the reader picks it up
  // next pass.
  ASSERT_EQ(writer.EndFragment(6, false), EndFragmentResult::kSuccess);
  reader.Drain(8);
  EXPECT_EQ(delegate.AllFragments(),
            (std::vector<std::string>{"committed", "suffix"}));
}

TEST(SharedRingBufferReaderTest, ReadsChunkWithNoPublishedFragments) {
  // With no published fragments, the reader still requests a rewrite.
  test::SharedRingBufferForTesting ring(4, 512);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);

  ASSERT_EQ(writer.BeginFragment(4, false).result,
            BeginFragmentResult::kSuccess);

  EXPECT_EQ(Internals::ConsumeNextPosition(&reader),
            ConsumeResult::kPositionSkipped);
  EXPECT_TRUE(delegate.chunks.empty());
  // Nothing came out, but the old owner is still stopped from publishing
  // behind the reader.
  EXPECT_EQ(
      ChunkStateOf(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0))),
      ChunkState::kRewriteRequested);
}

// ---------------------------------------------------------------------------
// Untrusted input.
// ---------------------------------------------------------------------------

TEST(SharedRingBufferReaderTest, UnknownFormat) {
  // An unknown format drops the payload but releases the chunk.
  test::SharedRingBufferForTesting ring(4, 256);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->TryReserveWritePos().chunk_pos, 0u);
  Internals::SetChunkStateWord(
      ring.get(), ChunkIndex::FromIndex(0),
      MakeDataStateWord(ChunkState::kComplete, ChunkFormat::kReservedRouting, 0,
                        3, kWriterB));

  EXPECT_EQ(Internals::ConsumeNextPosition(&reader),
            ConsumeResult::kPositionSkipped);
  EXPECT_EQ(reader.GetStats().unsupported_format_chunks, 1u);
  EXPECT_TRUE(delegate.chunks.empty());
  EXPECT_EQ(delegate.writers_with_data_loss, (std::vector<WriterID>{kWriterB}));
  // The loss was reported and the chunk remains usable.
  EXPECT_EQ(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0)),
            MakeFreeStateWord(1));
  EXPECT_EQ(reader.read_pos(), 1u);
}

TEST(SharedRingBufferReaderTest, SizesLargerThanChunk) {
  test::SharedRingBufferForTesting ring(4, 256);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->TryReserveWritePos().chunk_pos, 0u);
  // varint(0) takes one byte. 255 such entries do not fit in the 250-byte
  // payload area.
  Internals::SetChunkStateWord(
      ring.get(), ChunkIndex::FromIndex(0),
      MakeDataStateWord(ChunkState::kComplete, ChunkFormat::kTargetBuffer, 0,
                        255, kWriterB));

  EXPECT_EQ(Internals::ConsumeNextPosition(&reader),
            ConsumeResult::kPositionSkipped);
  EXPECT_EQ(reader.GetStats().malformed_chunks, 1u);
  EXPECT_TRUE(delegate.chunks.empty());
  EXPECT_EQ(delegate.writers_with_data_loss, (std::vector<WriterID>{kWriterB}));
  EXPECT_EQ(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0)),
            MakeFreeStateWord(1));
}

TEST(SharedRingBufferReaderTest, PayloadSizesOverlap) {
  // Payload and size varints must not overlap.
  test::SharedRingBufferForTesting ring(4, 256);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->TryReserveWritePos().chunk_pos, 0u);
  // The chunk has 250 bytes after its fixed header. A 249-byte fragment fits
  // by itself, but its two-byte size varint does not fit beside it.
  WriteFragmentSizeReversed(ring->chunk_at(ChunkIndex::FromIndex(0)) + 256,
                            249);
  Internals::SetChunkStateWord(
      ring.get(), ChunkIndex::FromIndex(0),
      MakeDataStateWord(ChunkState::kComplete, ChunkFormat::kTargetBuffer, 0, 1,
                        kWriterB));

  EXPECT_EQ(Internals::ConsumeNextPosition(&reader),
            ConsumeResult::kPositionSkipped);
  EXPECT_EQ(reader.GetStats().malformed_chunks, 1u);
  EXPECT_EQ(delegate.writers_with_data_loss, (std::vector<WriterID>{kWriterB}));
  EXPECT_EQ(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0)),
            MakeFreeStateWord(1));
}

TEST(SharedRingBufferReaderTest, NonMinimalSizeUsesActualDirectoryBytes) {
  // A three-byte size entry leaves room for 247 payload bytes.
  // 248 would fit with a minimal two-byte entry, but overlaps this directory.
  for (uint32_t size : {247u, 248u}) {
    SCOPED_TRACE(size);
    test::SharedRingBufferForTesting ring(2, 256);
    RecordingDelegate delegate;
    SharedRingBufferReader reader(ring.get(), &delegate);
    ASSERT_EQ(ring->TryReserveWritePos().chunk_pos, 0u);
    uint8_t* chunk = ring->chunk_at(ChunkIndex::FromIndex(0));
    StoreTargetBufferId(chunk, kBuffer);
    memset(chunk + kTargetBufferPayloadOffset, 'x', size);
    chunk[255] = static_cast<uint8_t>(size);
    chunk[254] = 0x81;
    chunk[253] = 0x00;
    Internals::SetChunkStateWord(
        ring.get(), ChunkIndex::FromIndex(0),
        MakeDataStateWord(ChunkState::kComplete, ChunkFormat::kTargetBuffer, 0,
                          1, kWriterA));

    EXPECT_EQ(reader.Drain(1).positions_consumed, 1u);
    if (size == 247) {
      EXPECT_EQ(delegate.AllFragments(),
                std::vector<std::string>{std::string(size, 'x')});
      EXPECT_TRUE(delegate.writers_with_data_loss.empty());
      EXPECT_EQ(reader.GetStats().malformed_chunks, 0u);
    } else {
      EXPECT_TRUE(delegate.chunks.empty());
      EXPECT_EQ(delegate.writers_with_data_loss,
                std::vector<WriterID>{kWriterA});
      EXPECT_EQ(reader.GetStats().malformed_chunks, 1u);
    }
    EXPECT_EQ(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0)),
              MakeFreeStateWord(1));
  }
}

TEST(SharedRingBufferReaderTest, CumulativeSizeOverflow) {
  test::SharedRingBufferForTesting ring(4, 512);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->TryReserveWritePos().chunk_pos, 0u);
  // Two fragments claiming 400 bytes each in a chunk that has 506 bytes of
  // payload area, of which four go to the size varints.
  uint8_t* chunk = ring->chunk_at(ChunkIndex::FromIndex(0));
  uint8_t* sizes_begin = chunk + 512;
  sizes_begin = WriteFragmentSizeReversed(sizes_begin, 400);
  WriteFragmentSizeReversed(sizes_begin, 400);
  Internals::SetChunkStateWord(
      ring.get(), ChunkIndex::FromIndex(0),
      MakeDataStateWord(ChunkState::kComplete, ChunkFormat::kTargetBuffer, 0, 2,
                        kWriterB));

  EXPECT_EQ(Internals::ConsumeNextPosition(&reader),
            ConsumeResult::kPositionSkipped);
  EXPECT_EQ(reader.GetStats().malformed_chunks, 1u);
  EXPECT_TRUE(delegate.chunks.empty());
  EXPECT_EQ(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0)),
            MakeFreeStateWord(1));
}

TEST(SharedRingBufferReaderTest, UnterminatedFragmentSize) {
  test::SharedRingBufferForTesting ring(4, 512);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->TryReserveWritePos().chunk_pos, 0u);
  for (uint32_t i = 0; i < kMaxFragmentSizeVarIntBytes; ++i)
    ring->chunk_at(ChunkIndex::FromIndex(0))[511 - i] = 0x80;
  Internals::SetChunkStateWord(
      ring.get(), ChunkIndex::FromIndex(0),
      MakeDataStateWord(ChunkState::kComplete, ChunkFormat::kTargetBuffer, 0, 1,
                        kWriterB));

  EXPECT_EQ(Internals::ConsumeNextPosition(&reader),
            ConsumeResult::kPositionSkipped);
  EXPECT_EQ(reader.GetStats().malformed_chunks, 1u);
  EXPECT_TRUE(delegate.chunks.empty());
  EXPECT_EQ(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0)),
            MakeFreeStateWord(1));
}

// A count larger than the writer actually wrote is a producer claim like any
// other. It cannot make the reader leave the chunk or spin. It just decodes the
// stale bytes underneath as extra fragments.
TEST(SharedRingBufferReaderTest, InflatedFragmentCount) {
  test::SharedRingBufferForTesting ring(4, 512);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->TryReserveWritePos().chunk_pos, 0u);
  Internals::SetChunkStateWord(
      ring.get(), ChunkIndex::FromIndex(0),
      MakeDataStateWord(ChunkState::kComplete, ChunkFormat::kTargetBuffer, 0, 8,
                        kWriterB));

  EXPECT_EQ(Internals::ConsumeNextPosition(&reader), ConsumeResult::kChunkRead);
  // Over a zero-filled mapping the inflated count decodes as extra zero-length
  // fragments. The checks make the walk memory-safe. They do not pretend to
  // make its contents true.
  ASSERT_EQ(delegate.chunks.size(), 1u);
  EXPECT_EQ(delegate.chunks[0].fragments.size(), 8u);
  for (const std::string& fragment : delegate.chunks[0].fragments)
    EXPECT_TRUE(fragment.empty());
  EXPECT_EQ(reader.GetStats().malformed_chunks, 0u);
  EXPECT_EQ(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0)),
            MakeFreeStateWord(1));
  EXPECT_EQ(reader.read_pos(), 1u);
}

TEST(SharedRingBufferReaderTest, MalformedBeingWrittenChunk) {
  // A malformed BeingWritten chunk is still marked RewriteRequested.
  test::SharedRingBufferForTesting ring(4, 256);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->TryReserveWritePos().chunk_pos, 0u);
  Internals::SetChunkStateWord(
      ring.get(), ChunkIndex::FromIndex(0),
      MakeDataStateWord(ChunkState::kBeingWritten, ChunkFormat::kTargetBuffer,
                        0, 255, kWriterB));

  EXPECT_EQ(Internals::ConsumeNextPosition(&reader),
            ConsumeResult::kPositionSkipped);
  EXPECT_EQ(reader.GetStats().malformed_chunks, 1u);
  // Malformed bytes may be dropped. What must not happen is leaving a
  // BeingWritten owner able to publish behind the reader.
  EXPECT_EQ(
      ChunkStateOf(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0))),
      ChunkState::kRewriteRequested);
}

TEST(SharedRingBufferReaderTest, ReservedStateStopsRing) {
  test::SharedRingBufferForTesting ring(4, 256);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->TryReserveWritePos().chunk_pos, 0u);
  const uint32_t reserved_word = 0x00000005u;
  Internals::SetChunkStateWord(ring.get(), ChunkIndex::FromIndex(0),
                               reserved_word);

  EXPECT_EQ(Internals::ConsumeNextPosition(&reader),
            ConsumeResult::kProtocolError);
  EXPECT_TRUE(reader.has_protocol_error());
  EXPECT_EQ(reader.read_pos(), 0u);
  EXPECT_EQ(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0)),
            reserved_word);

  // The ring buffer is never read again, and a drain over it changes nothing.
  const SharedRingBufferReader::DrainResult result = reader.Drain(8);
  EXPECT_EQ(result.positions_consumed, 0u);
  EXPECT_EQ(result.last_result, ConsumeResult::kProtocolError);
  EXPECT_EQ(Internals::GetReadPos(ring.get()), 0u);
}

TEST(SharedRingBufferReaderTest, ForeignFreeWordStopsRing) {
  test::SharedRingBufferForTesting ring(4, 256);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->TryReserveWritePos().chunk_pos, 0u);
  // Position 0 expects Free(0). Only the reader writes a free word, and
  // it derives it from the position it is consuming, so no legal execution puts
  // this here.
  const uint32_t wrong_wrap = MakeFreeStateWord(3);
  Internals::SetChunkStateWord(ring.get(), ChunkIndex::FromIndex(0),
                               wrong_wrap);

  EXPECT_EQ(Internals::ConsumeNextPosition(&reader),
            ConsumeResult::kProtocolError);
  EXPECT_TRUE(reader.has_protocol_error());
  EXPECT_EQ(reader.read_pos(), 0u);
  EXPECT_EQ(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0)),
            wrong_wrap);
}

// The control and fragment-count fields of a Free word are zero. A word using
// them may be malformed or belong to an unknown ABI. The reader must stop
// without reclaiming a word it does not understand.
TEST(SharedRingBufferReaderTest, FreeReservedBitsStopRing) {
  test::SharedRingBufferForTesting ring(4, 256);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->TryReserveWritePos().chunk_pos, 0u);
  // The wrap count is position 0's, but a reserved bit is set.
  const uint32_t reserved_bit_word = MakeFreeStateWord(0) | (1u << 8);
  Internals::SetChunkStateWord(ring.get(), ChunkIndex::FromIndex(0),
                               reserved_bit_word);

  EXPECT_EQ(Internals::ConsumeNextPosition(&reader),
            ConsumeResult::kProtocolError);
  EXPECT_TRUE(reader.has_protocol_error());
  EXPECT_EQ(reader.read_pos(), 0u);
  EXPECT_EQ(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0)),
            reserved_bit_word);
}

// RewriteAcknowledged has one canonical word: the state bits and nothing else.
// A word with those state bits plus payload bits is not one a legal writer can
// leave behind, so the reader stops on it instead of reclaiming the chunk, and
// the forged word stays where it was.
TEST(SharedRingBufferReaderTest, ForgedRewriteAckStopsRing) {
  test::SharedRingBufferForTesting ring(4, 256);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->TryReserveWritePos().chunk_pos, 0u);
  const uint32_t forged = kRewriteAcknowledgedStateWord | kFlagDataLoss;
  Internals::SetChunkStateWord(ring.get(), ChunkIndex::FromIndex(0), forged);

  EXPECT_EQ(Internals::ConsumeNextPosition(&reader),
            ConsumeResult::kProtocolError);
  EXPECT_TRUE(reader.has_protocol_error());
  EXPECT_EQ(reader.read_pos(), 0u);
  EXPECT_EQ(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0)), forged);
}

// A writer reserves a position only after seeing fewer than num_chunks
// outstanding, so write_pos further ahead than that cannot come from a legal
// producer. Once the mapping is writable by another process, believing it would
// mean consuming positions that were never reserved, one drain pass after
// another, for as long as the producer keeps write_pos there.
TEST(SharedRingBufferReaderTest, TooManyOutstandingStopsRing) {
  test::SharedRingBufferForTesting ring(4, 256);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  // Four chunks, five outstanding positions.
  Internals::SetWritePos(ring.get(), 5);

  EXPECT_EQ(Internals::ConsumeNextPosition(&reader),
            ConsumeResult::kProtocolError);
  EXPECT_TRUE(reader.has_protocol_error());
  EXPECT_EQ(reader.read_pos(), 0u);
  EXPECT_EQ(Internals::GetReadPos(ring.get()), 0u);
  EXPECT_TRUE(delegate.chunks.empty());
}

// The boundary itself is legal and must still be drained: num_chunks
// outstanding positions is exactly a full ring buffer.
TEST(SharedRingBufferReaderTest, FullRingIsLegal) {
  test::SharedRingBufferForTesting ring(4, 256);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);
  for (uint32_t i = 0; i < 4; ++i) {
    ASSERT_TRUE(WriteFragment(&writer, "x"));
    writer.FinishCurrentChunk();
  }
  ASSERT_EQ(ring->LoadWritePosRelaxed(), 4u);

  const SharedRingBufferReader::DrainResult result = reader.Drain(8);
  EXPECT_FALSE(reader.has_protocol_error());
  EXPECT_EQ(result.positions_consumed, 4u);
  EXPECT_EQ(delegate.chunks.size(), 4u);
}

// ---------------------------------------------------------------------------
// Flags and routing survive the trip.
// ---------------------------------------------------------------------------

TEST(SharedRingBufferReaderTest, FlagsAndTargetBufferDelivered) {
  test::SharedRingBufferForTesting ring(8, 512);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  SharedRingBufferWriter first = MakeWriter(ring.get(), kWriterA, 11);
  ASSERT_TRUE(WriteFragment(&first, "head", /*continues_from_prev=*/false,
                            /*continues_on_next=*/true));
  ASSERT_TRUE(WriteFragment(&first, "tail", /*continues_from_prev=*/true,
                            /*continues_on_next=*/false));
  first.FinishCurrentChunk();

  SharedRingBufferWriter second = MakeWriter(ring.get(), kWriterB, 22);
  ASSERT_TRUE(WriteFragment(&second, "other"));
  second.FinishCurrentChunk();

  reader.Drain(16);
  ASSERT_EQ(delegate.chunks.size(), 3u);

  EXPECT_EQ(delegate.chunks[0].target_buffer, 11);
  EXPECT_EQ(delegate.chunks[0].payload_flags, kFlagContinuesOnNextChunk);
  EXPECT_EQ(delegate.chunks[1].target_buffer, 11);
  EXPECT_EQ(delegate.chunks[1].payload_flags, kFlagContinuesFromPrevChunk);
  EXPECT_EQ(delegate.chunks[2].target_buffer, 22);
  EXPECT_EQ(delegate.chunks[2].writer_id, kWriterB);
  EXPECT_EQ(delegate.chunks[2].payload_flags, 0u);
}

TEST(SharedRingBufferReaderTest, DiscardsAllPublishedFragmentsAfterLoss) {
  for (bool writer_is_appending : {false, true}) {
    SCOPED_TRACE(writer_is_appending);
    test::SharedRingBufferForTesting ring(4, 512);
    RecordingDelegate delegate;
    SharedRingBufferReader reader(ring.get(), &delegate);
    SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);

    ASSERT_TRUE(WriteFragment(&writer, "before"));
    writer.RecordDataLoss();
    ASSERT_TRUE(WriteFragment(&writer, "after"));
    ASSERT_EQ(ring->LoadWritePosRelaxed(), 1u);
    if (writer_is_appending) {
      const auto range = writer.BeginFragment(5, false);
      ASSERT_EQ(range.result, BeginFragmentResult::kSuccess);
      memcpy(range.begin, "clean", 5);
    } else {
      ASSERT_EQ(writer.FinishCurrentChunk(), EndFragmentResult::kSuccess);
    }

    const auto result = reader.Drain(1);
    EXPECT_EQ(result.positions_consumed, 1u);
    EXPECT_EQ(result.last_result, ConsumeResult::kPositionSkipped);
    EXPECT_TRUE(delegate.chunks.empty());
    EXPECT_EQ(delegate.writers_with_data_loss, std::vector<WriterID>{kWriterA});

    if (writer_is_appending) {
      const uint32_t word =
          ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0));
      EXPECT_EQ(ChunkStateOf(word), ChunkState::kRewriteRequested);
      EXPECT_EQ(NumFragmentsOf(word), 2u);
      // The loss has been reported. The unpublished fragment can move to a
      // clean chunk without repeating it.
      ASSERT_EQ(writer.EndFragment(5, false), EndFragmentResult::kSuccess);
      EXPECT_EQ(writer.GetStats().relocations, 1u);
    } else {
      ASSERT_TRUE(WriteFragment(&writer, "clean"));
    }
    ASSERT_EQ(writer.FinishCurrentChunk(), EndFragmentResult::kSuccess);
    reader.Drain(4);
    EXPECT_EQ(delegate.AllFragments(), std::vector<std::string>{"clean"});
    EXPECT_EQ(delegate.writers_with_data_loss.size(), 1u);
    EXPECT_FALSE(reader.has_protocol_error());
  }
}

TEST(SharedRingBufferReaderTest, PendingLossSurvivesReaderWinningPublication) {
  test::SharedRingBufferForTesting ring(4, 512);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);
  ASSERT_TRUE(WriteFragment(&writer, "before"));
  writer.RecordDataLoss();
  const auto range = writer.BeginFragment(5, false);
  ASSERT_EQ(range.result, BeginFragmentResult::kSuccess);
  memcpy(range.begin, "after", 5);

  // The new fragment and loss flag are still unpublished. The reader may
  // deliver the older fragment, then request a rewrite.
  EXPECT_EQ(reader.Drain(1).last_result, ConsumeResult::kChunkRead);
  EXPECT_EQ(delegate.AllFragments(), std::vector<std::string>{"before"});
  EXPECT_TRUE(delegate.writers_with_data_loss.empty());

  ASSERT_EQ(writer.EndFragment(5, false), EndFragmentResult::kSuccess);
  EXPECT_EQ(writer.GetStats().relocations, 1u);
  EXPECT_NE(
      ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(1)) & kFlagDataLoss,
      0u);
  EXPECT_EQ(reader.Drain(1).last_result, ConsumeResult::kPositionSkipped);
  EXPECT_EQ(delegate.AllFragments(), std::vector<std::string>{"before"});
  EXPECT_EQ(delegate.writers_with_data_loss, std::vector<WriterID>{kWriterA});

  ASSERT_TRUE(WriteFragment(&writer, "clean"));
  ASSERT_EQ(writer.FinishCurrentChunk(), EndFragmentResult::kSuccess);
  reader.Drain(4);
  EXPECT_EQ(delegate.AllFragments(),
            (std::vector<std::string>{"before", "clean"}));
  EXPECT_EQ(delegate.writers_with_data_loss.size(), 1u);
}

// A writer that recorded a loss, claimed a chunk for its next packet and then
// gave up on it publishes Complete(0) with kFlagDataLoss. Once the reader
// reclaims that chunk no writer can carry the flag any further, so the reader
// reports it itself. The writer's next chunk starts clean, so the loss is
// reported exactly once.
TEST(SharedRingBufferReaderTest, DataLossOnEmptyCompleteChunk) {
  test::SharedRingBufferForTesting ring(4, 256);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);

  writer.RecordDataLoss();
  ASSERT_EQ(writer.BeginFragment(4, false).result,
            BeginFragmentResult::kSuccess);
  ASSERT_EQ(writer.FinishCurrentChunk(), EndFragmentResult::kSuccess);
  ASSERT_EQ(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0)),
            MakeDataStateWord(ChunkState::kComplete, ChunkFormat::kTargetBuffer,
                              kFlagDataLoss, 0, kWriterA));

  EXPECT_EQ(Internals::ConsumeNextPosition(&reader),
            ConsumeResult::kPositionSkipped);
  EXPECT_TRUE(delegate.chunks.empty());
  EXPECT_EQ(delegate.writers_with_data_loss, (std::vector<WriterID>{kWriterA}));
  EXPECT_EQ(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0)),
            MakeFreeStateWord(1));

  // The writer's reuse of that chunk fails and its replacement does not repeat
  // the flag.
  ASSERT_TRUE(WriteFragment(&writer, "after"));
  writer.FinishCurrentChunk();
  reader.Drain(8);
  ASSERT_EQ(delegate.chunks.size(), 1u);
  EXPECT_EQ(delegate.chunks[0].payload_flags, 0u);
  EXPECT_EQ(delegate.writers_with_data_loss.size(), 1u);
}

// The reader wins against BeingWritten(0) with kFlagDataLoss. It takes no
// fragments and reports no loss: the writer still owns the chunk and moves the
// flag to the relocated fragment, so the loss surfaces once, on that chunk.
TEST(SharedRingBufferReaderTest, DataLossFollowsRelocatedFragment) {
  test::SharedRingBufferForTesting ring(4, 256);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);

  writer.RecordDataLoss();
  const auto range = writer.BeginFragment(6, false);
  ASSERT_EQ(range.result, BeginFragmentResult::kSuccess);
  memcpy(range.begin, "suffix", 6);

  EXPECT_EQ(Internals::ConsumeNextPosition(&reader),
            ConsumeResult::kPositionSkipped);
  EXPECT_TRUE(delegate.chunks.empty());
  EXPECT_TRUE(delegate.writers_with_data_loss.empty());
  EXPECT_EQ(
      ChunkStateOf(ring->LoadChunkStateWordAcquire(ChunkIndex::FromIndex(0))),
      ChunkState::kRewriteRequested);

  ASSERT_EQ(writer.EndFragment(6, false), EndFragmentResult::kSuccess);
  EXPECT_EQ(writer.GetStats().relocations, 1u);
  writer.FinishCurrentChunk();

  // Position 0 was the scraped chunk. Position 1 holds the relocated fragment.
  reader.Drain(8);
  EXPECT_TRUE(delegate.chunks.empty());
  EXPECT_EQ(delegate.writers_with_data_loss, (std::vector<WriterID>{kWriterA}));
}

// The smallest supported ring buffer reuses both chunks across traversals.
TEST(SharedRingBufferReaderTest, TwoChunkRing) {
  test::SharedRingBufferForTesting ring(2, 256);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);

  std::vector<std::string> expected;
  for (uint32_t i = 0; i < 5; ++i) {
    const std::string bytes = "packet-" + std::to_string(i);
    ASSERT_TRUE(WriteFragment(&writer, bytes)) << i;
    writer.FinishCurrentChunk();
    expected.push_back(bytes);
    const SharedRingBufferReader::DrainResult result = reader.Drain(4);
    EXPECT_EQ(result.positions_consumed, 1u) << i;
  }
  EXPECT_EQ(delegate.AllFragments(), expected);
  EXPECT_EQ(Internals::ConsumeNextPosition(&reader), ConsumeResult::kNoData);
}

// An aligned, non-power-of-two chunk size, end to end through writer and
// reader.
TEST(SharedRingBufferReaderTest, NonPowerOfTwoChunkSize) {
  test::SharedRingBufferForTesting ring(4, 260);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);
  SharedRingBufferWriter writer = MakeWriter(ring.get(), kWriterA, kBuffer);

  // 300 bytes does not fit a 260-byte chunk, so it arrives as two fragments in
  // two chunks - the split the upper layer would mark with the continuation
  // flags.
  const std::string head(252, 'h');
  const std::string tail(48, 't');
  ASSERT_TRUE(WriteFragment(&writer, head, /*continues_from_prev=*/false,
                            /*continues_on_next=*/true));
  ASSERT_TRUE(WriteFragment(&writer, tail, /*continues_from_prev=*/true,
                            /*continues_on_next=*/false));
  writer.FinishCurrentChunk();

  reader.Drain(8);
  ASSERT_EQ(delegate.chunks.size(), 2u);
  EXPECT_EQ(delegate.chunks[0].payload_flags, kFlagContinuesOnNextChunk);
  EXPECT_EQ(delegate.chunks[1].payload_flags, kFlagContinuesFromPrevChunk);
  EXPECT_EQ(delegate.AllFragments(), (std::vector<std::string>{head, tail}));
}

TEST(SharedRingBufferReaderTest, ReservationOrderAcrossWriters) {
  test::SharedRingBufferForTesting ring(8, 256);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  // Interleave two writers so their chunks land at alternating positions.
  SharedRingBufferWriter a = MakeWriter(ring.get(), kWriterA, kBuffer);
  SharedRingBufferWriter b = MakeWriter(ring.get(), kWriterB, kBuffer);
  std::vector<std::string> expected;
  for (uint32_t i = 0; i < 4; ++i) {
    // Each fragment fills its chunk, so every write consumes a new position.
    const std::string from_a = "a" + std::to_string(i) + std::string(240, '.');
    const std::string from_b = "b" + std::to_string(i) + std::string(240, '.');
    ASSERT_TRUE(WriteFragment(&a, from_a));
    ASSERT_TRUE(WriteFragment(&b, from_b));
    expected.push_back(from_a);
    expected.push_back(from_b);
    reader.Drain(16);
  }
  a.FinishCurrentChunk();
  b.FinishCurrentChunk();
  reader.Drain(16);

  EXPECT_EQ(delegate.AllFragments(), expected);
}

}  // namespace
}  // namespace perfetto::tracing_v2
