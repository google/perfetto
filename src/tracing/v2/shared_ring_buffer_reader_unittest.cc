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

#include "perfetto/ext/base/no_destructor.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/shared_ring_buffer_writer.h"
#include "src/tracing/v2/tracing_v2_abi.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::tracing_v2 {
namespace {

using Outcome = SharedRingBufferReader::Outcome;

constexpr WriterID kWriterA = 7;
constexpr WriterID kWriterB = 8;
constexpr BufferID kBuffer = 0x1234;

class NoopSharedRingBufferWriterDelegate
    : public SharedRingBufferWriter::Delegate {
 public:
  void NotifyReader() override {}
};

SharedRingBufferWriter::Delegate* GetNoopSharedRingBufferWriterDelegate() {
  static base::NoDestructor<NoopSharedRingBufferWriterDelegate> delegate;
  return &delegate.ref();
}

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

SharedRingBufferWriter::Outcome WriteFragment(SharedRingBufferWriter* writer,
                                              const std::string& bytes,
                                              bool continues_from_prev = false,
                                              bool continues_on_next = false) {
  const SharedRingBufferWriter::FragmentSpan span = writer->OpenFragment(
      static_cast<uint32_t>(bytes.size()), continues_from_prev);
  if (span.outcome != SharedRingBufferWriter::Outcome::kOk)
    return span.outcome;
  memcpy(span.begin, bytes.data(), bytes.size());
  return writer->CloseFragment(static_cast<uint32_t>(bytes.size()),
                               continues_on_next);
}

// ---------------------------------------------------------------------------
// The nominal path.
// ---------------------------------------------------------------------------

TEST(SharedRingBufferReaderTest, EmptyRingHasNoData) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  EXPECT_EQ(reader.ResolveNextPosition(), Outcome::kNoData);
  EXPECT_EQ(reader.read_pos(), 0u);
  const SharedRingBufferReader::DrainResult result = reader.Drain(16);
  EXPECT_EQ(result.positions_resolved, 0u);
  EXPECT_FALSE(result.needs_another_drain());
}

TEST(SharedRingBufferReaderTest,
     ConsumesACompleteChunkAndFreesItForTheNextTraversal) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);
  SharedRingBufferWriter writer(ring.get(), kWriterA, kBuffer,
                                BufferExhaustedPolicy::kDrop,
                                GetNoopSharedRingBufferWriterDelegate());

  ASSERT_EQ(WriteFragment(&writer, "alpha"),
            SharedRingBufferWriter::Outcome::kOk);
  ASSERT_EQ(WriteFragment(&writer, "beta"),
            SharedRingBufferWriter::Outcome::kOk);
  writer.FinishCurrentChunk();

  EXPECT_EQ(reader.ResolveNextPosition(), Outcome::kChunkRead);
  EXPECT_EQ(reader.read_pos(), 1u);
  ASSERT_EQ(delegate.chunks.size(), 1u);
  EXPECT_EQ(delegate.chunks[0].writer_id, kWriterA);
  EXPECT_EQ(delegate.chunks[0].target_buffer, kBuffer);
  EXPECT_EQ(delegate.chunks[0].fragments,
            (std::vector<std::string>{"alpha", "beta"}));

  // The chunk is now tagged for the traversal after the one just resolved.
  EXPECT_EQ(ring->LoadChunkStateWord(0), MakeFreeStateWord(1));
  EXPECT_EQ(reader.ResolveNextPosition(), Outcome::kNoData);
}

TEST(SharedRingBufferReaderTest, DrainPublishesReadPosOncePerPass) {
  auto ring = SharedRingBuffer::Create(8, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  // Four writers, one chunk each, so the pass has four positions to resolve.
  std::vector<std::unique_ptr<SharedRingBufferWriter>> writers;
  for (uint32_t i = 0; i < 4; ++i) {
    writers.push_back(std::make_unique<SharedRingBufferWriter>(
        ring.get(), static_cast<WriterID>(10 + i), kBuffer,
        BufferExhaustedPolicy::kDrop, GetNoopSharedRingBufferWriterDelegate()));
    ASSERT_EQ(WriteFragment(writers.back().get(), "x"),
              SharedRingBufferWriter::Outcome::kOk);
  }
  // The shared read_pos has not moved yet: ResolveNextPosition() does not
  // publish it.
  EXPECT_EQ(ring->read_pos_for_testing(), 0u);

  const SharedRingBufferReader::DrainResult result = reader.Drain(16);
  EXPECT_EQ(result.positions_resolved, 4u);
  EXPECT_EQ(result.last_outcome, Outcome::kNoData);
  EXPECT_FALSE(result.needs_another_drain());
  EXPECT_EQ(ring->read_pos_for_testing(), 4u);
  EXPECT_EQ(delegate.chunks.size(), 4u);
}

TEST(SharedRingBufferReaderTest, DrainStopsAtItsBudgetAndSaysWorkMayRemain) {
  auto ring = SharedRingBuffer::Create(8, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  std::vector<std::unique_ptr<SharedRingBufferWriter>> writers;
  for (uint32_t i = 0; i < 4; ++i) {
    writers.push_back(std::make_unique<SharedRingBufferWriter>(
        ring.get(), static_cast<WriterID>(10 + i), kBuffer,
        BufferExhaustedPolicy::kDrop, GetNoopSharedRingBufferWriterDelegate()));
    ASSERT_EQ(WriteFragment(writers.back().get(), "x"),
              SharedRingBufferWriter::Outcome::kOk);
  }

  const SharedRingBufferReader::DrainResult first = reader.Drain(2);
  EXPECT_EQ(first.positions_resolved, 2u);
  EXPECT_TRUE(first.needs_another_drain());
  EXPECT_EQ(ring->read_pos_for_testing(), 2u);

  const SharedRingBufferReader::DrainResult second = reader.Drain(16);
  EXPECT_EQ(second.positions_resolved, 2u);
  EXPECT_FALSE(second.needs_another_drain());
  EXPECT_EQ(delegate.chunks.size(), 4u);
}

// ---------------------------------------------------------------------------
// Holes.
// ---------------------------------------------------------------------------

TEST(SharedRingBufferReaderTest,
     UnclaimedPositionIsAHoleAndPreparesTheNextTraversal) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  // A writer reserved position 0 and never claimed it.
  ASSERT_EQ(ring->TryReserveWritePos().position, 0u);

  EXPECT_EQ(reader.ResolveNextPosition(), Outcome::kSkipped);
  EXPECT_EQ(reader.read_pos(), 1u);
  EXPECT_EQ(reader.num_positions_skipped(), 1u);
  EXPECT_TRUE(delegate.chunks.empty());
  EXPECT_EQ(ring->LoadChunkStateWord(0), MakeFreeStateWord(1));
}

TEST(SharedRingBufferReaderTest,
     RewriteRequestedIsSkippedWithoutTouchingTheWord) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  // Chunk 0 is left mid-rewrite by a writer that has not come back, and a later
  // position maps onto it.
  const uint32_t being_written = MakeDataStateWord(
      ChunkState::kBeingWritten, ChunkFormat::kTargetBuffer, 0, 0, kWriterB);
  ASSERT_TRUE(ring->TryAcquireChunkForWriting(0, being_written));
  uint32_t observed = being_written;
  ASSERT_TRUE(ring->TryRequestRewrite(0, &observed));
  const uint32_t marked = ring->LoadChunkStateWord(0);

  ASSERT_EQ(ring->TryReserveWritePos().position, 0u);
  EXPECT_EQ(reader.ResolveNextPosition(), Outcome::kSkipped);
  EXPECT_EQ(reader.read_pos(), 1u);
  // Only the owning writer may leave that state, so the reader left it alone.
  EXPECT_EQ(ring->LoadChunkStateWord(0), marked);
  EXPECT_EQ(reader.num_positions_skipped(), 1u);
}

TEST(SharedRingBufferReaderTest,
     RewriteAcknowledgedIsReclaimedForResolvedPosition) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  const uint32_t being_written = MakeDataStateWord(
      ChunkState::kBeingWritten, ChunkFormat::kTargetBuffer, 0, 0, kWriterB);
  ASSERT_TRUE(ring->TryAcquireChunkForWriting(0, being_written));
  uint32_t observed = being_written;
  ASSERT_TRUE(ring->TryRequestRewrite(0, &observed));
  ASSERT_TRUE(ring->TryAcknowledgeRewrite(
      0, ReplaceChunkState(being_written, ChunkState::kRewriteRequested)));

  ASSERT_EQ(ring->TryReserveWritePos().position, 0u);
  EXPECT_EQ(reader.ResolveNextPosition(), Outcome::kSkipped);
  EXPECT_EQ(ring->LoadChunkStateWord(0), MakeFreeStateWord(1));
}

// ---------------------------------------------------------------------------
// Scraping a live writer.
// ---------------------------------------------------------------------------

TEST(SharedRingBufferReaderTest, TakesTheCommittedPrefixOfALiveWriter) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);
  SharedRingBufferWriter writer(ring.get(), kWriterA, kBuffer,
                                BufferExhaustedPolicy::kDrop,
                                GetNoopSharedRingBufferWriterDelegate());

  ASSERT_EQ(WriteFragment(&writer, "committed"),
            SharedRingBufferWriter::Outcome::kOk);
  // The writer is inside the chunk with a fragment still open.
  const SharedRingBufferWriter::FragmentSpan span =
      writer.OpenFragment(6, false);
  ASSERT_EQ(span.outcome, SharedRingBufferWriter::Outcome::kOk);
  memcpy(span.begin, "suffix", 6);

  EXPECT_EQ(reader.ResolveNextPosition(), Outcome::kChunkRead);
  EXPECT_EQ(reader.num_scrapes(), 1u);
  ASSERT_EQ(delegate.chunks.size(), 1u);
  // Only what the writer had published, and nothing of the open fragment.
  EXPECT_EQ(delegate.chunks[0].fragments,
            (std::vector<std::string>{"committed"}));
  EXPECT_EQ(ChunkStateOf(ring->LoadChunkStateWord(0)),
            ChunkState::kRewriteRequested);

  // The writer relocates its suffix, and the reader picks it up next pass.
  ASSERT_EQ(writer.CloseFragment(6, false),
            SharedRingBufferWriter::Outcome::kOk);
  reader.Drain(8);
  EXPECT_EQ(delegate.AllFragments(),
            (std::vector<std::string>{"committed", "suffix"}));
}

TEST(SharedRingBufferReaderTest, ScrapeWithNothingCommittedStillMarksTheChunk) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);
  SharedRingBufferWriter writer(ring.get(), kWriterA, kBuffer,
                                BufferExhaustedPolicy::kDrop,
                                GetNoopSharedRingBufferWriterDelegate());

  ASSERT_EQ(writer.OpenFragment(4, false).outcome,
            SharedRingBufferWriter::Outcome::kOk);

  EXPECT_EQ(reader.ResolveNextPosition(), Outcome::kSkipped);
  EXPECT_TRUE(delegate.chunks.empty());
  // Nothing came out, but the old owner is still stopped from publishing
  // behind the reader.
  EXPECT_EQ(ChunkStateOf(ring->LoadChunkStateWord(0)),
            ChunkState::kRewriteRequested);
}

// The reader's mark loses to the writer's publication. The speculative copy is
// discarded and the position is retried, so the fragments come out exactly
// once and in order.
TEST(SharedRingBufferReaderTest,
     PublicationBeatingTheScrapeIsReadOnTheNextAttempt) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);
  SharedRingBufferWriter writer(ring.get(), kWriterA, kBuffer,
                                BufferExhaustedPolicy::kDrop,
                                GetNoopSharedRingBufferWriterDelegate());

  ASSERT_EQ(WriteFragment(&writer, "first"),
            SharedRingBufferWriter::Outcome::kOk);
  const SharedRingBufferWriter::FragmentSpan span =
      writer.OpenFragment(6, false);
  ASSERT_EQ(span.outcome, SharedRingBufferWriter::Outcome::kOk);
  memcpy(span.begin, "second", 6);

  // Publish from inside the reader, after it has copied the prefix and just
  // before it tries to claim it.
  bool published = false;
  reader.SetArbitrationHookForTesting([&] {
    if (published)
      return;
    published = true;
    EXPECT_EQ(writer.CloseFragment(6, false),
              SharedRingBufferWriter::Outcome::kOk);
  });

  EXPECT_EQ(reader.ResolveNextPosition(), Outcome::kRetryLater);
  EXPECT_EQ(reader.read_pos(), 0u);
  EXPECT_TRUE(delegate.chunks.empty());

  reader.SetArbitrationHookForTesting(nullptr);
  EXPECT_EQ(reader.ResolveNextPosition(), Outcome::kChunkRead);
  EXPECT_EQ(delegate.AllFragments(),
            (std::vector<std::string>{"first", "second"}));
  // No relocation happened: the writer won, so there was nothing to move.
  EXPECT_EQ(writer.num_relocations(), 0u);
  EXPECT_EQ(reader.num_scrapes(), 0u);
}

TEST(SharedRingBufferReaderTest, ReuseBeatingTheReclaimIsHandledAsAScrape) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);
  SharedRingBufferWriter writer(ring.get(), kWriterA, kBuffer,
                                BufferExhaustedPolicy::kDrop,
                                GetNoopSharedRingBufferWriterDelegate());

  ASSERT_EQ(WriteFragment(&writer, "first"),
            SharedRingBufferWriter::Outcome::kOk);

  // The writer takes the Complete chunk back just as the reader is about to
  // reclaim it. The reader leaves the position unchanged and takes the
  // committed prefix on its next attempt.
  bool reused = false;
  reader.SetArbitrationHookForTesting([&] {
    if (reused)
      return;
    reused = true;
    ASSERT_EQ(writer.OpenFragment(3, false).outcome,
              SharedRingBufferWriter::Outcome::kOk);
  });

  EXPECT_EQ(reader.ResolveNextPosition(), Outcome::kRetryLater);
  EXPECT_EQ(reader.read_pos(), 0u);
  EXPECT_TRUE(delegate.chunks.empty());

  reader.SetArbitrationHookForTesting(nullptr);
  EXPECT_EQ(reader.ResolveNextPosition(), Outcome::kChunkRead);
  EXPECT_EQ(delegate.AllFragments(), (std::vector<std::string>{"first"}));
  EXPECT_EQ(reader.num_scrapes(), 1u);
  EXPECT_EQ(ChunkStateOf(ring->LoadChunkStateWord(0)),
            ChunkState::kRewriteRequested);
}

// ---------------------------------------------------------------------------
// Untrusted input.
// ---------------------------------------------------------------------------

TEST(SharedRingBufferReaderTest,
     UnknownFormatDropsThePayloadButReleasesTheChunk) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->TryReserveWritePos().position, 0u);
  ring->SetStateWordForTesting(
      0, MakeDataStateWord(ChunkState::kComplete, ChunkFormat::kReservedRouting,
                           0, 3, kWriterB));

  EXPECT_EQ(reader.ResolveNextPosition(), Outcome::kSkipped);
  EXPECT_EQ(reader.num_unknown_format_chunks(), 1u);
  EXPECT_TRUE(delegate.chunks.empty());
  EXPECT_EQ(delegate.writers_with_data_loss, (std::vector<WriterID>{kWriterB}));
  // The loss was reported and the chunk remains usable.
  EXPECT_EQ(ring->LoadChunkStateWord(0), MakeFreeStateWord(1));
  EXPECT_EQ(reader.read_pos(), 1u);
}

TEST(SharedRingBufferReaderTest,
     DirectoryLargerThanTheChunkIsRejectedBoundedly) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->TryReserveWritePos().position, 0u);
  // varint(0) takes one byte. 255 such entries do not fit in the 250-byte
  // payload area.
  ring->SetStateWordForTesting(
      0, MakeDataStateWord(ChunkState::kComplete, ChunkFormat::kTargetBuffer, 0,
                           255, kWriterB));

  EXPECT_EQ(reader.ResolveNextPosition(), Outcome::kSkipped);
  EXPECT_EQ(reader.num_malformed_chunks(), 1u);
  EXPECT_TRUE(delegate.chunks.empty());
  EXPECT_EQ(delegate.writers_with_data_loss, (std::vector<WriterID>{kWriterB}));
  EXPECT_EQ(ring->LoadChunkStateWord(0), MakeFreeStateWord(1));
}

TEST(SharedRingBufferReaderTest, PayloadAndDirectoryMustNotOverlap) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->TryReserveWritePos().position, 0u);
  // The chunk has 250 bytes after its fixed header. A 249-byte fragment fits
  // by itself, but its two-byte size varint does not fit beside it.
  WriteFragmentSize(ring->chunk_at(0) + 256, 249);
  ring->SetStateWordForTesting(
      0, MakeDataStateWord(ChunkState::kComplete, ChunkFormat::kTargetBuffer, 0,
                           1, kWriterB));

  EXPECT_EQ(reader.ResolveNextPosition(), Outcome::kSkipped);
  EXPECT_EQ(reader.num_malformed_chunks(), 1u);
  EXPECT_EQ(delegate.writers_with_data_loss, (std::vector<WriterID>{kWriterB}));
  EXPECT_EQ(ring->LoadChunkStateWord(0), MakeFreeStateWord(1));
}

TEST(SharedRingBufferReaderTest, SpeculativeParseDoesNotReportDataLoss) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->TryReserveWritePos().position, 0u);
  const uint32_t being_written = MakeDataStateWord(
      ChunkState::kBeingWritten, ChunkFormat::kTargetBuffer, 0, 255, kWriterB);
  ring->SetStateWordForTesting(0, being_written);
  reader.SetArbitrationHookForTesting([&] {
    uint32_t observed = being_written;
    ASSERT_TRUE(ring->TrySetChunkComplete(
        0, &observed, ReplaceChunkState(being_written, ChunkState::kComplete)));
  });

  EXPECT_EQ(reader.ResolveNextPosition(), Outcome::kRetryLater);
  EXPECT_EQ(reader.num_malformed_chunks(), 0u);
  EXPECT_TRUE(delegate.writers_with_data_loss.empty());

  reader.SetArbitrationHookForTesting(nullptr);
  EXPECT_EQ(reader.ResolveNextPosition(), Outcome::kSkipped);
  EXPECT_EQ(reader.num_malformed_chunks(), 1u);
  EXPECT_EQ(delegate.writers_with_data_loss, (std::vector<WriterID>{kWriterB}));
}

TEST(SharedRingBufferReaderTest,
     CumulativeFragmentSizesBeyondTheChunkAreRejected) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->TryReserveWritePos().position, 0u);
  // Two fragments claiming 400 bytes each in a chunk that has 506 bytes of
  // payload area, of which four go to the directory.
  uint8_t* chunk = ring->chunk_at(0);
  uint8_t* directory_begin = chunk + 512;
  directory_begin = WriteFragmentSize(directory_begin, 400);
  WriteFragmentSize(directory_begin, 400);
  ring->SetStateWordForTesting(
      0, MakeDataStateWord(ChunkState::kComplete, ChunkFormat::kTargetBuffer, 0,
                           2, kWriterB));

  EXPECT_EQ(reader.ResolveNextPosition(), Outcome::kSkipped);
  EXPECT_EQ(reader.num_malformed_chunks(), 1u);
  EXPECT_TRUE(delegate.chunks.empty());
  EXPECT_EQ(ring->LoadChunkStateWord(0), MakeFreeStateWord(1));
}

TEST(SharedRingBufferReaderTest, UnterminatedFragmentSizeVarIntIsRejected) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->TryReserveWritePos().position, 0u);
  for (uint32_t i = 0; i < kMaxFragmentSizeVarIntBytes; ++i)
    ring->chunk_at(0)[511 - i] = 0x80;
  ring->SetStateWordForTesting(
      0, MakeDataStateWord(ChunkState::kComplete, ChunkFormat::kTargetBuffer, 0,
                           1, kWriterB));

  EXPECT_EQ(reader.ResolveNextPosition(), Outcome::kSkipped);
  EXPECT_EQ(reader.num_malformed_chunks(), 1u);
  EXPECT_TRUE(delegate.chunks.empty());
  EXPECT_EQ(ring->LoadChunkStateWord(0), MakeFreeStateWord(1));
}

// A count larger than the writer actually wrote is a producer claim like any
// other. It cannot make the reader leave the chunk or spin; it just decodes the
// stale bytes underneath as extra fragments.
TEST(SharedRingBufferReaderTest, InflatedFragmentCountStaysInsideTheChunk) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->TryReserveWritePos().position, 0u);
  ring->SetStateWordForTesting(
      0, MakeDataStateWord(ChunkState::kComplete, ChunkFormat::kTargetBuffer, 0,
                           8, kWriterB));

  EXPECT_EQ(reader.ResolveNextPosition(), Outcome::kChunkRead);
  // Over a zero-filled mapping the inflated count decodes as extra zero-length
  // fragments. The checks make the walk memory-safe; they do not pretend to
  // make its contents true.
  ASSERT_EQ(delegate.chunks.size(), 1u);
  EXPECT_EQ(delegate.chunks[0].fragments.size(), 8u);
  for (const std::string& fragment : delegate.chunks[0].fragments)
    EXPECT_TRUE(fragment.empty());
  EXPECT_EQ(reader.num_malformed_chunks(), 0u);
  EXPECT_EQ(ring->LoadChunkStateWord(0), MakeFreeStateWord(1));
  EXPECT_EQ(reader.read_pos(), 1u);
}

TEST(SharedRingBufferReaderTest,
     MalformedBeingWrittenChunkIsStillRewriteRequested) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->TryReserveWritePos().position, 0u);
  ring->SetStateWordForTesting(
      0, MakeDataStateWord(ChunkState::kBeingWritten,
                           ChunkFormat::kTargetBuffer, 0, 255, kWriterB));

  EXPECT_EQ(reader.ResolveNextPosition(), Outcome::kSkipped);
  EXPECT_EQ(reader.num_malformed_chunks(), 1u);
  // Malformed bytes may be dropped. What must not happen is leaving an
  // BeingWritten owner able to publish behind the reader.
  EXPECT_EQ(ChunkStateOf(ring->LoadChunkStateWord(0)),
            ChunkState::kRewriteRequested);
}

TEST(SharedRingBufferReaderTest,
     ReservedStateStopsTheRingWithoutChangingAnything) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->TryReserveWritePos().position, 0u);
  const uint32_t reserved_word = 0x00000005u;
  ring->SetStateWordForTesting(0, reserved_word);

  EXPECT_EQ(reader.ResolveNextPosition(), Outcome::kProtocolError);
  EXPECT_TRUE(reader.has_protocol_error());
  EXPECT_EQ(reader.read_pos(), 0u);
  EXPECT_EQ(ring->LoadChunkStateWord(0), reserved_word);

  // The ring is never read again, and a drain over it changes nothing.
  const SharedRingBufferReader::DrainResult result = reader.Drain(8);
  EXPECT_EQ(result.positions_resolved, 0u);
  EXPECT_EQ(result.last_outcome, Outcome::kProtocolError);
  EXPECT_EQ(ring->read_pos_for_testing(), 0u);
}

TEST(SharedRingBufferReaderTest, FreeWordFromAnotherTraversalStopsTheRing) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->TryReserveWritePos().position, 0u);
  // Position 0 expects Free(0). Only the reader writes a free word, and
  // it derives it from the position it is resolving, so no legal execution puts
  // this here.
  const uint32_t wrong_wrap = MakeFreeStateWord(3);
  ring->SetStateWordForTesting(0, wrong_wrap);

  EXPECT_EQ(reader.ResolveNextPosition(), Outcome::kProtocolError);
  EXPECT_TRUE(reader.has_protocol_error());
  EXPECT_EQ(reader.read_pos(), 0u);
  EXPECT_EQ(ring->LoadChunkStateWord(0), wrong_wrap);
}

// The control and fragment-count fields of a Free word are zero. A word using
// them is some future ABI's, not garbage to be normalized away: the reader
// must not consume the position as if it understood the word.
TEST(SharedRingBufferReaderTest, FreeWordWithReservedBitsSetStopsTheRing) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->TryReserveWritePos().position, 0u);
  // The wrap count is position 0's, but a reserved bit is set.
  const uint32_t reserved_bit_word = MakeFreeStateWord(0) | (1u << 8);
  ring->SetStateWordForTesting(0, reserved_bit_word);

  EXPECT_EQ(reader.ResolveNextPosition(), Outcome::kProtocolError);
  EXPECT_TRUE(reader.has_protocol_error());
  EXPECT_EQ(reader.read_pos(), 0u);
  EXPECT_EQ(ring->LoadChunkStateWord(0), reserved_bit_word);
}

// A word with the RewriteAcknowledged state bits but a nonzero payload is not
// the word that transition may leave from. The reclaim's failed exact-value
// CAS hands the reader the forged word itself, and the ring stops on it.
TEST(SharedRingBufferReaderTest, ForgedRewriteAcknowledgedWordStopsTheRing) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  ASSERT_EQ(ring->TryReserveWritePos().position, 0u);
  const uint32_t forged = kRewriteAcknowledgedStateWord | kFlagDataLoss;
  ring->SetStateWordForTesting(0, forged);

  EXPECT_EQ(reader.ResolveNextPosition(), Outcome::kProtocolError);
  EXPECT_TRUE(reader.has_protocol_error());
  EXPECT_EQ(reader.read_pos(), 0u);
  EXPECT_EQ(ring->LoadChunkStateWord(0), forged);
}

// A writer reserves a position only after seeing fewer than num_chunks
// outstanding, so write_pos further ahead than that cannot come from a legal
// producer. Once the mapping is writable by another process, believing it would
// mean resolving positions that were never reserved, one drain pass after
// another, for as long as the producer keeps write_pos there.
TEST(SharedRingBufferReaderTest, TooManyOutstandingPositionsStopsTheRing) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  // Four chunks, five outstanding positions.
  ring->SetWritePosForTesting(5);

  EXPECT_EQ(reader.ResolveNextPosition(), Outcome::kProtocolError);
  EXPECT_TRUE(reader.has_protocol_error());
  EXPECT_EQ(reader.read_pos(), 0u);
  EXPECT_EQ(ring->read_pos_for_testing(), 0u);
  EXPECT_TRUE(delegate.chunks.empty());
}

// The boundary itself is legal and must still be drained: num_chunks
// outstanding positions is exactly a full ring.
TEST(SharedRingBufferReaderTest, AFullRingIsNotAnImpossibleDistance) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  SharedRingBufferWriter writer(ring.get(), kWriterA, kBuffer,
                                BufferExhaustedPolicy::kDrop,
                                GetNoopSharedRingBufferWriterDelegate());
  for (uint32_t i = 0; i < 4; ++i) {
    ASSERT_EQ(WriteFragment(&writer, "x"),
              SharedRingBufferWriter::Outcome::kOk);
    writer.FinishCurrentChunk();
  }
  ASSERT_EQ(ring->LoadWritePos(), 4u);

  const SharedRingBufferReader::DrainResult result = reader.Drain(8);
  EXPECT_FALSE(reader.has_protocol_error());
  EXPECT_EQ(result.positions_resolved, 4u);
  EXPECT_EQ(delegate.chunks.size(), 4u);
}

// ---------------------------------------------------------------------------
// Flags and routing survive the trip.
// ---------------------------------------------------------------------------

TEST(SharedRingBufferReaderTest,
     ContinuationFlagsAndTargetBufferReachTheDelegate) {
  auto ring = SharedRingBuffer::Create(8, 512);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  SharedRingBufferWriter first(ring.get(), kWriterA, 11,
                               BufferExhaustedPolicy::kDrop,
                               GetNoopSharedRingBufferWriterDelegate());
  first.RecordDataLoss();
  ASSERT_EQ(WriteFragment(&first, "head", /*continues_from_prev=*/false,
                          /*continues_on_next=*/true),
            SharedRingBufferWriter::Outcome::kOk);
  ASSERT_EQ(WriteFragment(&first, "tail", /*continues_from_prev=*/true,
                          /*continues_on_next=*/false),
            SharedRingBufferWriter::Outcome::kOk);
  first.FinishCurrentChunk();

  SharedRingBufferWriter second(ring.get(), kWriterB, 22,
                                BufferExhaustedPolicy::kDrop,
                                GetNoopSharedRingBufferWriterDelegate());
  ASSERT_EQ(WriteFragment(&second, "other"),
            SharedRingBufferWriter::Outcome::kOk);
  second.FinishCurrentChunk();

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

// A one-chunk ring is legal: every position maps to chunk 0 and the ring
// alternates between one outstanding reservation and empty.
TEST(SharedRingBufferReaderTest, OneChunkRingRoundTrips) {
  auto ring = SharedRingBuffer::Create(1, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);
  SharedRingBufferWriter writer(ring.get(), kWriterA, kBuffer,
                                BufferExhaustedPolicy::kDrop,
                                GetNoopSharedRingBufferWriterDelegate());

  std::vector<std::string> expected;
  for (uint32_t i = 0; i < 5; ++i) {
    const std::string bytes = "packet-" + std::to_string(i);
    ASSERT_EQ(WriteFragment(&writer, bytes),
              SharedRingBufferWriter::Outcome::kOk)
        << i;
    writer.FinishCurrentChunk();
    expected.push_back(bytes);
    const SharedRingBufferReader::DrainResult result = reader.Drain(4);
    EXPECT_EQ(result.positions_resolved, 1u) << i;
  }
  EXPECT_EQ(delegate.AllFragments(), expected);
  EXPECT_EQ(reader.ResolveNextPosition(), Outcome::kNoData);
}

// An aligned, non-power-of-two chunk size, end to end through writer and
// reader.
TEST(SharedRingBufferReaderTest, NonPowerOfTwoChunkSizeRoundTrips) {
  auto ring = SharedRingBuffer::Create(4, 260);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);
  SharedRingBufferWriter writer(ring.get(), kWriterA, kBuffer,
                                BufferExhaustedPolicy::kDrop,
                                GetNoopSharedRingBufferWriterDelegate());

  // 300 bytes does not fit a 260-byte chunk, so it arrives as two fragments in
  // two chunks - the split the upper layer would mark with the continuation
  // flags.
  const std::string head(252, 'h');
  const std::string tail(48, 't');
  ASSERT_EQ(WriteFragment(&writer, head, /*continues_from_prev=*/false,
                          /*continues_on_next=*/true),
            SharedRingBufferWriter::Outcome::kOk);
  ASSERT_EQ(WriteFragment(&writer, tail, /*continues_from_prev=*/true,
                          /*continues_on_next=*/false),
            SharedRingBufferWriter::Outcome::kOk);
  writer.FinishCurrentChunk();

  reader.Drain(8);
  ASSERT_EQ(delegate.chunks.size(), 2u);
  EXPECT_EQ(delegate.chunks[0].payload_flags, kFlagContinuesOnNextChunk);
  EXPECT_EQ(delegate.chunks[1].payload_flags, kFlagContinuesFromPrevChunk);
  EXPECT_EQ(delegate.AllFragments(), (std::vector<std::string>{head, tail}));
}

TEST(SharedRingBufferReaderTest,
     FragmentsComeOutInReservationOrderAcrossWriters) {
  auto ring = SharedRingBuffer::Create(8, 256);
  ASSERT_NE(ring, nullptr);
  RecordingDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);

  // Interleave two writers so their chunks land at alternating positions.
  SharedRingBufferWriter a(ring.get(), kWriterA, kBuffer,
                           BufferExhaustedPolicy::kDrop,
                           GetNoopSharedRingBufferWriterDelegate());
  SharedRingBufferWriter b(ring.get(), kWriterB, kBuffer,
                           BufferExhaustedPolicy::kDrop,
                           GetNoopSharedRingBufferWriterDelegate());
  std::vector<std::string> expected;
  for (uint32_t i = 0; i < 4; ++i) {
    // Each fragment fills its chunk, so every write consumes a new position.
    const std::string from_a = "a" + std::to_string(i) + std::string(240, '.');
    const std::string from_b = "b" + std::to_string(i) + std::string(240, '.');
    ASSERT_EQ(WriteFragment(&a, from_a), SharedRingBufferWriter::Outcome::kOk);
    ASSERT_EQ(WriteFragment(&b, from_b), SharedRingBufferWriter::Outcome::kOk);
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
