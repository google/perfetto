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

#include <memory>
#include <string>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/ext/base/no_destructor.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/tracing_v2_abi.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::tracing_v2 {
namespace {

constexpr WriterID kWriterA = 7;
constexpr WriterID kWriterB = 8;
constexpr BufferID kBuffer = 0x1234;

class CountingSharedRingBufferWriterDelegate
    : public SharedRingBufferWriter::Delegate {
 public:
  void NotifyReader() override { ++num_notifications; }

  uint32_t num_notifications = 0;
};

class ReleasingSharedRingBufferWriterDelegate
    : public SharedRingBufferWriter::Delegate {
 public:
  explicit ReleasingSharedRingBufferWriterDelegate(SharedRingBuffer* ring)
      : ring_(ring) {}

  void NotifyReader() override {
    ++num_notifications;
    const uint32_t read_pos = ring_->read_pos_for_testing();
    const uint32_t chunk_index =
        ChunkIndexOfPosition(read_pos, ring_->num_chunks());
    uint32_t observed = ring_->LoadChunkStateWord(chunk_index);
    ASSERT_EQ(ChunkStateOf(observed), ChunkState::kComplete);
    ASSERT_TRUE(ring_->TryReleaseCompleteChunkAsFree(read_pos, &observed));
    ring_->PublishReadPos(read_pos + 1);
  }

  uint32_t num_notifications = 0;

 private:
  SharedRingBuffer* const ring_;
};

class NoopSharedRingBufferWriterDelegate
    : public SharedRingBufferWriter::Delegate {
 public:
  void NotifyReader() override {}
};

SharedRingBufferWriter::Delegate* GetNoopSharedRingBufferWriterDelegate() {
  static base::NoDestructor<NoopSharedRingBufferWriterDelegate> delegate;
  return &delegate.ref();
}

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

DecodedChunk Decode(SharedRingBuffer* ring, uint32_t chunk_index) {
  const uint8_t* chunk = ring->chunk_at(chunk_index);
  const uint32_t word = ring->LoadChunkStateWord(chunk_index);
  DecodedChunk decoded;
  decoded.state = ChunkStateOf(word);
  if (!HasDataFields(decoded.state))
    return decoded;

  decoded.writer_id = WriterIdOf(word);
  decoded.target_buffer = LoadTargetBufferId(chunk);
  decoded.payload_flags = PayloadFlagsOf(word);

  const uint8_t* directory_cursor = chunk + ring->chunk_size();
  uint32_t offset = kTargetBufferPayloadOffset;
  for (uint32_t i = 0; i < NumFragmentsOf(word); ++i) {
    uint32_t size = 0;
    const bool valid = ReadFragmentSize(chunk + kTargetBufferPayloadOffset,
                                        &directory_cursor, &size);
    EXPECT_TRUE(valid);
    if (!valid)
      return decoded;
    decoded.fragments.emplace_back(
        reinterpret_cast<const char*>(chunk + offset), size);
    offset += size;
  }
  return decoded;
}

// Writes one fragment holding |bytes| and publishes it.
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

// Plays the reader's part of the scrape: marks whatever the writer currently
// holds as rewrite-requested and reports what the marked prefix was.
uint32_t MarkForRewrite(SharedRingBuffer* ring, uint32_t chunk_index) {
  uint32_t observed = ring->LoadChunkStateWord(chunk_index);
  EXPECT_EQ(ChunkStateOf(observed), ChunkState::kBeingWritten);
  const uint32_t taken = NumFragmentsOf(observed);
  EXPECT_TRUE(ring->TryRequestRewrite(chunk_index, &observed));
  return taken;
}

// ---------------------------------------------------------------------------
// Fragments and the directory.
// ---------------------------------------------------------------------------

TEST(SharedRingBufferWriterTest, WritesPayloadUpAndSizesDown) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  SharedRingBufferWriter writer(ring.get(), kWriterA, kBuffer,
                                BufferExhaustedPolicy::kDrop,
                                GetNoopSharedRingBufferWriterDelegate());

  ASSERT_EQ(WriteFragment(&writer, std::string(5, 'a')),
            SharedRingBufferWriter::Outcome::kOk);
  ASSERT_EQ(WriteFragment(&writer, std::string(200, 'b')),
            SharedRingBufferWriter::Outcome::kOk);
  ASSERT_EQ(WriteFragment(&writer, std::string(3, 'c')),
            SharedRingBufferWriter::Outcome::kOk);

  // A 256-byte target-buffer chunk, byte for byte.
  const uint8_t* chunk = ring->chunk_at(0);
  EXPECT_EQ(chunk[255], 5u);
  EXPECT_EQ(chunk[254], 0xc8u);
  EXPECT_EQ(chunk[253], 1u);
  EXPECT_EQ(chunk[252], 3u);

  const DecodedChunk decoded = Decode(ring.get(), 0);
  EXPECT_EQ(decoded.state, ChunkState::kComplete);
  EXPECT_EQ(decoded.writer_id, kWriterA);
  EXPECT_EQ(decoded.target_buffer, kBuffer);
  ASSERT_EQ(decoded.fragments.size(), 3u);
  EXPECT_EQ(decoded.fragments[0], std::string(5, 'a'));
  EXPECT_EQ(decoded.fragments[1], std::string(200, 'b'));
  EXPECT_EQ(decoded.fragments[2], std::string(3, 'c'));
}

TEST(SharedRingBufferWriterTest, FragmentSizesAtTheInterestingBoundaries) {
  // These sizes straddle the one- and two-byte varint boundary.
  auto ring = SharedRingBuffer::Create(8, 512);
  ASSERT_NE(ring, nullptr);
  SharedRingBufferWriter writer(ring.get(), kWriterA, kBuffer,
                                BufferExhaustedPolicy::kDrop,
                                GetNoopSharedRingBufferWriterDelegate());

  const uint32_t kSizes[] = {0, 1, 127, 128, 255, 256};
  std::vector<std::string> expected;
  for (uint32_t size : kSizes) {
    const std::string bytes(size, static_cast<char>('A' + (size % 26)));
    ASSERT_EQ(WriteFragment(&writer, bytes),
              SharedRingBufferWriter::Outcome::kOk)
        << size;
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

TEST(SharedRingBufferWriterTest,
     LargestFragmentFitsExactlyAndOneMoreByteDoesNot) {
  auto ring = SharedRingBuffer::Create(4, 256);
  ASSERT_NE(ring, nullptr);
  // 256 - 6 header bytes - varint(248), which takes two bytes.
  constexpr uint32_t kLargest = 248;

  SharedRingBufferWriter writer(ring.get(), kWriterA, kBuffer,
                                BufferExhaustedPolicy::kDrop,
                                GetNoopSharedRingBufferWriterDelegate());
  const SharedRingBufferWriter::FragmentSpan span =
      writer.OpenFragment(kLargest, false);
  ASSERT_EQ(span.outcome, SharedRingBufferWriter::Outcome::kOk);
  EXPECT_EQ(static_cast<uint32_t>(span.end - span.begin), kLargest);
  memset(span.begin, 'z', kLargest);
  ASSERT_EQ(writer.CloseFragment(kLargest, false),
            SharedRingBufferWriter::Outcome::kOk);

  // The payload and the directory have met: the chunk cannot take another
  // fragment, not even an empty one.
  EXPECT_EQ(writer.MaxFragmentSizeInCurrentChunk(), 0u);
  ASSERT_EQ(Decode(ring.get(), 0).fragments.size(), 1u);
  EXPECT_EQ(Decode(ring.get(), 0).fragments[0].size(), kLargest);

  // One byte more than any chunk of this size could ever hold is a caller
  // bug, not backpressure, and says so.
  SharedRingBufferWriter other(ring.get(), kWriterB, kBuffer,
                               BufferExhaustedPolicy::kDrop,
                               GetNoopSharedRingBufferWriterDelegate());
  EXPECT_EQ(other.OpenFragment(kLargest + 1, false).outcome,
            SharedRingBufferWriter::Outcome::kTooLarge);
}

TEST(SharedRingBufferWriterTest, ChunkClosesAt255FragmentsEvenWithSpaceLeft) {
  auto ring = SharedRingBuffer::Create(4, 32768);
  ASSERT_NE(ring, nullptr);
  SharedRingBufferWriter writer(ring.get(), kWriterA, kBuffer,
                                BufferExhaustedPolicy::kDrop,
                                GetNoopSharedRingBufferWriterDelegate());

  for (uint32_t i = 0; i < kMaxFragmentsPerChunk; ++i)
    ASSERT_EQ(WriteFragment(&writer, ""), SharedRingBufferWriter::Outcome::kOk)
        << i;

  const DecodedChunk first = Decode(ring.get(), 0);
  EXPECT_EQ(first.fragments.size(), kMaxFragmentsPerChunk);
  // Thousands of payload bytes are still free; the eight-bit count is what
  // ended the chunk.
  EXPECT_EQ(writer.MaxFragmentSizeInCurrentChunk(), 0u);

  ASSERT_EQ(WriteFragment(&writer, "x"), SharedRingBufferWriter::Outcome::kOk);
  const DecodedChunk second = Decode(ring.get(), 1);
  ASSERT_EQ(second.fragments.size(), 1u);
  EXPECT_EQ(second.fragments[0], "x");
}

TEST(SharedRingBufferWriterTest, LargeChunkSupportsAFragmentLargerThanUint16) {
  constexpr uint32_t kBigChunk = 128 * 1024;
  constexpr uint32_t kLargest = kBigChunk - 9;
  auto ring = SharedRingBuffer::Create(2, kBigChunk);
  ASSERT_NE(ring, nullptr);
  SharedRingBufferWriter writer(ring.get(), kWriterA, kBuffer,
                                BufferExhaustedPolicy::kDrop,
                                GetNoopSharedRingBufferWriterDelegate());

  EXPECT_EQ(writer.OpenFragment(kLargest + 1, false).outcome,
            SharedRingBufferWriter::Outcome::kTooLarge);

  const SharedRingBufferWriter::FragmentSpan span =
      writer.OpenFragment(1, false);
  ASSERT_EQ(span.outcome, SharedRingBufferWriter::Outcome::kOk);
  EXPECT_EQ(static_cast<uint32_t>(span.end - span.begin), kLargest);
  memset(span.begin, 'a', kLargest);
  ASSERT_EQ(writer.CloseFragment(kLargest, false),
            SharedRingBufferWriter::Outcome::kOk);

  const DecodedChunk decoded = Decode(ring.get(), 0);
  ASSERT_EQ(decoded.fragments.size(), 1u);
  EXPECT_EQ(decoded.fragments[0], std::string(kLargest, 'a'));
}

// An aligned, non-power-of-two chunk size, end to end through the writer.
TEST(SharedRingBufferWriterTest, WritesAndDecodesInANonPowerOfTwoChunk) {
  auto ring = SharedRingBuffer::Create(4, 260);
  ASSERT_NE(ring, nullptr);
  SharedRingBufferWriter writer(ring.get(), kWriterA, kBuffer,
                                BufferExhaustedPolicy::kDrop,
                                GetNoopSharedRingBufferWriterDelegate());

  // 260 - 6 header bytes - the two-byte varint for 252.
  constexpr uint32_t kLargest = 252;
  EXPECT_EQ(writer.OpenFragment(kLargest + 1, false).outcome,
            SharedRingBufferWriter::Outcome::kTooLarge);
  ASSERT_EQ(WriteFragment(&writer, std::string(kLargest, 'z')),
            SharedRingBufferWriter::Outcome::kOk);
  EXPECT_EQ(writer.MaxFragmentSizeInCurrentChunk(), 0u);

  ASSERT_EQ(WriteFragment(&writer, "next"),
            SharedRingBufferWriter::Outcome::kOk);
  ASSERT_EQ(Decode(ring.get(), 0).fragments.size(), 1u);
  EXPECT_EQ(Decode(ring.get(), 0).fragments[0].size(), kLargest);
  const DecodedChunk second = Decode(ring.get(), 1);
  ASSERT_EQ(second.fragments.size(), 1u);
  EXPECT_EQ(second.fragments[0], "next");
}

// ---------------------------------------------------------------------------
// Chunk reuse and the payload flags.
// ---------------------------------------------------------------------------

TEST(SharedRingBufferWriterTest, ReusesItsOwnCompleteChunkWhileItCanTakeMore) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  SharedRingBufferWriter writer(ring.get(), kWriterA, kBuffer,
                                BufferExhaustedPolicy::kDrop,
                                GetNoopSharedRingBufferWriterDelegate());

  ASSERT_EQ(WriteFragment(&writer, "one"),
            SharedRingBufferWriter::Outcome::kOk);
  ASSERT_EQ(WriteFragment(&writer, "two"),
            SharedRingBufferWriter::Outcome::kOk);

  // Both fragments are in the same physical chunk, and only one position was
  // consumed.
  EXPECT_EQ(ring->LoadWritePos(), 1u);
  const DecodedChunk decoded = Decode(ring.get(), 0);
  ASSERT_EQ(decoded.fragments.size(), 2u);
  EXPECT_EQ(decoded.fragments[0], "one");
  EXPECT_EQ(decoded.fragments[1], "two");
  EXPECT_EQ(ChunkStateOf(ring->LoadChunkStateWord(1)), ChunkState::kFree);
}

TEST(SharedRingBufferWriterTest, ReuseLosesToTheReaderReclaimingTheChunk) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  SharedRingBufferWriter writer(ring.get(), kWriterA, kBuffer,
                                BufferExhaustedPolicy::kDrop,
                                GetNoopSharedRingBufferWriterDelegate());

  ASSERT_EQ(WriteFragment(&writer, "one"),
            SharedRingBufferWriter::Outcome::kOk);

  // The reader consumes the Complete chunk before the writer takes it back.
  uint32_t observed = ring->LoadChunkStateWord(0);
  ASSERT_EQ(ChunkStateOf(observed), ChunkState::kComplete);
  ASSERT_TRUE(ring->TryReleaseCompleteChunkAsFree(0, &observed));
  ring->PublishReadPos(1);

  // The writer's reuse fails; it drops its handle and goes for a fresh chunk
  // rather than writing behind the reader.
  ASSERT_EQ(WriteFragment(&writer, "two"),
            SharedRingBufferWriter::Outcome::kOk);
  EXPECT_EQ(ChunkStateOf(ring->LoadChunkStateWord(0)), ChunkState::kFree);
  const DecodedChunk decoded = Decode(ring.get(), 1);
  ASSERT_EQ(decoded.fragments.size(), 1u);
  EXPECT_EQ(decoded.fragments[0], "two");
}

// A chunk published with "continues on next chunk" is never reused. Without
// that rule a later scrape could take a prefix ending in the middle of a
// packet.
TEST(SharedRingBufferWriterTest, ChunkCarryingContinuesOnNextIsNeverReused) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  SharedRingBufferWriter writer(ring.get(), kWriterA, kBuffer,
                                BufferExhaustedPolicy::kDrop,
                                GetNoopSharedRingBufferWriterDelegate());

  ASSERT_EQ(WriteFragment(&writer, "head", /*continues_from_prev=*/false,
                          /*continues_on_next=*/true),
            SharedRingBufferWriter::Outcome::kOk);
  ASSERT_EQ(WriteFragment(&writer, "tail", /*continues_from_prev=*/true,
                          /*continues_on_next=*/false),
            SharedRingBufferWriter::Outcome::kOk);

  const DecodedChunk first = Decode(ring.get(), 0);
  EXPECT_EQ(first.payload_flags, kFlagContinuesOnNextChunk);
  ASSERT_EQ(first.fragments.size(), 1u);
  EXPECT_EQ(first.fragments[0], "head");

  const DecodedChunk second = Decode(ring.get(), 1);
  EXPECT_EQ(second.payload_flags, kFlagContinuesFromPrevChunk);
  ASSERT_EQ(second.fragments.size(), 1u);
  EXPECT_EQ(second.fragments[0], "tail");
}

TEST(SharedRingBufferWriterTest, DataLossIsReportedOnTheNextChunkAndOnlyOnce) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  SharedRingBufferWriter writer(ring.get(), kWriterA, kBuffer,
                                BufferExhaustedPolicy::kDrop,
                                GetNoopSharedRingBufferWriterDelegate());

  writer.RecordDataLoss();
  ASSERT_EQ(WriteFragment(&writer, "after the gap"),
            SharedRingBufferWriter::Outcome::kOk);
  EXPECT_EQ(Decode(ring.get(), 0).payload_flags, kFlagDataLoss);

  // The chunk after it describes no gap of its own.
  ASSERT_EQ(WriteFragment(&writer, std::string(500, 'x')),
            SharedRingBufferWriter::Outcome::kOk);
  EXPECT_EQ(Decode(ring.get(), 1).payload_flags, 0u);
}

// ---------------------------------------------------------------------------
// Backpressure outcomes.
// ---------------------------------------------------------------------------

TEST(SharedRingBufferWriterTest, DropPolicyReportsFullWithoutBlocking) {
  auto ring = SharedRingBuffer::Create(2, 256);
  ASSERT_NE(ring, nullptr);
  SharedRingBufferWriter a(ring.get(), kWriterA, kBuffer,
                           BufferExhaustedPolicy::kDrop,
                           GetNoopSharedRingBufferWriterDelegate());
  SharedRingBufferWriter b(ring.get(), kWriterB, kBuffer,
                           BufferExhaustedPolicy::kDrop,
                           GetNoopSharedRingBufferWriterDelegate());

  // Two writers hold both chunks, so the ring is structurally full.
  ASSERT_EQ(a.OpenFragment(1, false).outcome,
            SharedRingBufferWriter::Outcome::kOk);
  ASSERT_EQ(b.OpenFragment(1, false).outcome,
            SharedRingBufferWriter::Outcome::kOk);

  SharedRingBufferWriter c(ring.get(), 11, kBuffer,
                           BufferExhaustedPolicy::kDrop,
                           GetNoopSharedRingBufferWriterDelegate());
  EXPECT_EQ(c.OpenFragment(1, false).outcome,
            SharedRingBufferWriter::Outcome::kFull);
  // Nothing was reserved, so a full ring costs no position.
  EXPECT_EQ(ring->LoadWritePos(), 2u);
  EXPECT_EQ(c.num_failed_claims(), 0u);
}

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX_BUT_NOT_QNX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)

TEST(SharedRingBufferWriterTest, FullRingNotifiesReaderBeforeWaiting) {
  auto ring = SharedRingBuffer::Create(1, 256);
  ASSERT_NE(ring, nullptr);

  SharedRingBufferWriter first(ring.get(), kWriterA, kBuffer,
                               BufferExhaustedPolicy::kDrop,
                               GetNoopSharedRingBufferWriterDelegate());
  ASSERT_EQ(WriteFragment(&first, "first"),
            SharedRingBufferWriter::Outcome::kOk);
  ASSERT_EQ(first.FinishCurrentChunk(), SharedRingBufferWriter::Outcome::kOk);

  ReleasingSharedRingBufferWriterDelegate delegate(ring.get());
  SharedRingBufferWriter second(ring.get(), kWriterB, kBuffer,
                                BufferExhaustedPolicy::kStall, &delegate);
  EXPECT_EQ(second.OpenFragment(1, false).outcome,
            SharedRingBufferWriter::Outcome::kOk);
  EXPECT_EQ(delegate.num_notifications, 1u);
}

TEST(SharedRingBufferWriterTest,
     StallThenDropDoesNotStallAgainBeforeAChunkIsAcquired) {
  auto ring = SharedRingBuffer::Create(1, 256);
  ASSERT_NE(ring, nullptr);

  SharedRingBufferWriter first(ring.get(), kWriterA, kBuffer,
                               BufferExhaustedPolicy::kDrop,
                               GetNoopSharedRingBufferWriterDelegate());
  ASSERT_EQ(WriteFragment(&first, "first"),
            SharedRingBufferWriter::Outcome::kOk);
  ASSERT_EQ(first.FinishCurrentChunk(), SharedRingBufferWriter::Outcome::kOk);

  ReleasingSharedRingBufferWriterDelegate delegate(ring.get());
  SharedRingBufferWriter second(ring.get(), kWriterB, kBuffer,
                                BufferExhaustedPolicy::kStallThenDrop,
                                &delegate);

  // RecordDataLoss() marks an active drop episode. Retrying while that loss is
  // still waiting to be reported must use kDrop rather than stall for another
  // 30 seconds.
  second.RecordDataLoss();
  EXPECT_EQ(second.OpenFragment(1, false).outcome,
            SharedRingBufferWriter::Outcome::kFull);
  EXPECT_EQ(delegate.num_notifications, 0u);

  uint32_t observed = ring->LoadChunkStateWord(0);
  ASSERT_TRUE(ring->TryReleaseCompleteChunkAsFree(0, &observed));
  ring->PublishReadPos(1);

  ASSERT_EQ(WriteFragment(&second, "after loss"),
            SharedRingBufferWriter::Outcome::kOk);
  EXPECT_EQ(Decode(ring.get(), 0).payload_flags, kFlagDataLoss);
  ASSERT_EQ(second.FinishCurrentChunk(), SharedRingBufferWriter::Outcome::kOk);

  // Publishing the loss ends the drop episode. The next exhausted acquisition
  // stalls again, which gives the delegate a chance to release the chunk.
  EXPECT_EQ(second.OpenFragment(1, false).outcome,
            SharedRingBufferWriter::Outcome::kOk);
  EXPECT_EQ(delegate.num_notifications, 1u);
}

#endif  // PERFETTO_OS_LINUX_BUT_NOT_QNX || PERFETTO_OS_ANDROID

// A chunk pinned by a writer that stopped mid-rewrite is not the same thing as
// a full ring: positions are available, but their chunks cannot be acquired.
TEST(SharedRingBufferWriterTest,
     PinnedChunksNotifyReaderAndReportNoChunkAvailable) {
  // The writer tries each physical chunk once. Those failed claims fill the
  // reservation window even though no writer acquired a chunk.
  auto ring = SharedRingBuffer::Create(8, 256);
  ASSERT_NE(ring, nullptr);

  // Pin every chunk in RewriteRequested, which only its owner may leave.
  // Claiming them directly leaves write_pos at zero, so there is capacity for
  // every reservation the writer below makes.
  for (uint32_t i = 0; i < ring->num_chunks(); ++i) {
    const uint32_t being_written = MakeDataStateWord(
        ChunkState::kBeingWritten, ChunkFormat::kTargetBuffer, 0, 0, kWriterB);
    ASSERT_TRUE(ring->TryAcquireChunkForWriting(i, being_written));
    uint32_t observed = being_written;
    ASSERT_TRUE(ring->TryRequestRewrite(i, &observed));
  }
  ASSERT_EQ(ring->LoadWritePos(), 0u);

  CountingSharedRingBufferWriterDelegate delegate;
  SharedRingBufferWriter writer(ring.get(), kWriterA, kBuffer,
                                BufferExhaustedPolicy::kDrop, &delegate);
  EXPECT_EQ(writer.OpenFragment(1, false).outcome,
            SharedRingBufferWriter::Outcome::kNoChunkAvailable);
  EXPECT_EQ(writer.num_failed_claims(), ring->num_chunks());
  EXPECT_EQ(ring->LoadWritePos(), ring->num_chunks());
  EXPECT_EQ(delegate.num_notifications, 1u);
}

// ---------------------------------------------------------------------------
// Relocation after a scrape.
// ---------------------------------------------------------------------------

TEST(SharedRingBufferWriterTest, ScrapeMovesOnlyTheUnpublishedSuffix) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  SharedRingBufferWriter writer(ring.get(), kWriterA, kBuffer,
                                BufferExhaustedPolicy::kDrop,
                                GetNoopSharedRingBufferWriterDelegate());

  ASSERT_EQ(WriteFragment(&writer, "published-one"),
            SharedRingBufferWriter::Outcome::kOk);
  ASSERT_EQ(WriteFragment(&writer, "published-two"),
            SharedRingBufferWriter::Outcome::kOk);

  // The writer takes the chunk back and fills a third fragment. The reader
  // arrives while it is inside and takes the two published ones.
  const SharedRingBufferWriter::FragmentSpan span =
      writer.OpenFragment(6, false);
  ASSERT_EQ(span.outcome, SharedRingBufferWriter::Outcome::kOk);
  memcpy(span.begin, "suffix", 6);
  EXPECT_EQ(MarkForRewrite(ring.get(), 0), 2u);

  ASSERT_EQ(writer.CloseFragment(6, false),
            SharedRingBufferWriter::Outcome::kOk);
  EXPECT_EQ(writer.num_relocations(), 1u);
  EXPECT_EQ(writer.num_fragments_dropped(), 0u);

  // The old chunk is acknowledged - the writer says nothing about who gets it
  // next - and only the suffix moved.
  EXPECT_EQ(ChunkStateOf(ring->LoadChunkStateWord(0)),
            ChunkState::kRewriteAcknowledged);
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

TEST(SharedRingBufferWriterTest,
     ScrapeWithNoPublishedPrefixCarriesTheFlagsAlong) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  SharedRingBufferWriter writer(ring.get(), kWriterA, kBuffer,
                                BufferExhaustedPolicy::kDrop,
                                GetNoopSharedRingBufferWriterDelegate());
  writer.RecordDataLoss();

  const SharedRingBufferWriter::FragmentSpan span =
      writer.OpenFragment(4, /*continues_from_prev=*/true);
  ASSERT_EQ(span.outcome, SharedRingBufferWriter::Outcome::kOk);
  memcpy(span.begin, "tail", 4);
  // The reader takes nothing: the writer has published no fragment yet.
  EXPECT_EQ(MarkForRewrite(ring.get(), 0), 0u);

  ASSERT_EQ(writer.CloseFragment(4, false),
            SharedRingBufferWriter::Outcome::kOk);

  const DecodedChunk replacement = Decode(ring.get(), 1);
  ASSERT_EQ(replacement.fragments.size(), 1u);
  EXPECT_EQ(replacement.fragments[0], "tail");
  // The reader took no beginning, so both flags describing it travel with the
  // relocated suffix.
  EXPECT_EQ(replacement.payload_flags,
            kFlagContinuesFromPrevChunk | kFlagDataLoss);
}

TEST(SharedRingBufferWriterTest, ScrapeWithNothingUnpublishedJustAcknowledges) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  SharedRingBufferWriter writer(ring.get(), kWriterA, kBuffer,
                                BufferExhaustedPolicy::kDrop,
                                GetNoopSharedRingBufferWriterDelegate());

  ASSERT_EQ(WriteFragment(&writer, "only"),
            SharedRingBufferWriter::Outcome::kOk);
  // Take the chunk back but add nothing, then let the reader scrape it.
  ASSERT_EQ(writer.OpenFragment(1, false).outcome,
            SharedRingBufferWriter::Outcome::kOk);
  EXPECT_EQ(MarkForRewrite(ring.get(), 0), 1u);

  // Releasing abandons the open fragment; there is nothing left to move.
  EXPECT_EQ(writer.FinishCurrentChunk(), SharedRingBufferWriter::Outcome::kOk);
  EXPECT_EQ(writer.num_fragments_dropped(), 0u);
  EXPECT_EQ(ChunkStateOf(ring->LoadChunkStateWord(0)),
            ChunkState::kRewriteAcknowledged);
  EXPECT_EQ(ChunkStateOf(ring->LoadChunkStateWord(1)), ChunkState::kFree);
}

// Acknowledging happens before the writer looks for replacement capacity. The
// other order would leave the old chunk occupied exactly when the ring is full,
// so every later traversal of it would burn a position.
TEST(SharedRingBufferWriterTest,
     RelocationWithNoCapacityDropsButFreesTheOldChunk) {
  auto ring = SharedRingBuffer::Create(2, 512);
  ASSERT_NE(ring, nullptr);
  SharedRingBufferWriter writer(ring.get(), kWriterA, kBuffer,
                                BufferExhaustedPolicy::kDrop,
                                GetNoopSharedRingBufferWriterDelegate());

  ASSERT_EQ(WriteFragment(&writer, "published"),
            SharedRingBufferWriter::Outcome::kOk);

  // Occupy the ring's only other chunk so no replacement can be had.
  SharedRingBufferWriter blocker(ring.get(), kWriterB, kBuffer,
                                 BufferExhaustedPolicy::kDrop,
                                 GetNoopSharedRingBufferWriterDelegate());
  ASSERT_EQ(blocker.OpenFragment(1, false).outcome,
            SharedRingBufferWriter::Outcome::kOk);

  const SharedRingBufferWriter::FragmentSpan span =
      writer.OpenFragment(4, false);
  ASSERT_EQ(span.outcome, SharedRingBufferWriter::Outcome::kOk);
  memcpy(span.begin, "lost", 4);
  EXPECT_EQ(MarkForRewrite(ring.get(), 0), 1u);

  EXPECT_EQ(writer.CloseFragment(4, false),
            SharedRingBufferWriter::Outcome::kRelocationDropped);
  EXPECT_EQ(writer.num_fragments_dropped(), 1u);
  // The old chunk is acknowledged and therefore reclaimable by the reader,
  // even though the data did not survive.
  EXPECT_EQ(ring->LoadChunkStateWord(0), kRewriteAcknowledgedStateWord);

  // The gap is reported on the next chunk this writer manages to publish.
  uint32_t observed = 0;
  ASSERT_TRUE(ring->TryReleaseRewriteAcknowledgedChunkAsFree(0, &observed));
  ring->PublishReadPos(1);
  ASSERT_EQ(WriteFragment(&writer, "next"),
            SharedRingBufferWriter::Outcome::kOk);
  EXPECT_EQ(Decode(ring.get(), 0).payload_flags, kFlagDataLoss);
}

TEST(SharedRingBufferWriterTest, DestructorPublishesWhateverIsHeld) {
  auto ring = SharedRingBuffer::Create(4, 512);
  ASSERT_NE(ring, nullptr);
  {
    SharedRingBufferWriter writer(ring.get(), kWriterA, kBuffer,
                                  BufferExhaustedPolicy::kDrop,
                                  GetNoopSharedRingBufferWriterDelegate());
    const SharedRingBufferWriter::FragmentSpan span =
        writer.OpenFragment(4, false);
    ASSERT_EQ(span.outcome, SharedRingBufferWriter::Outcome::kOk);
    memcpy(span.begin, "kept", 4);
    ASSERT_EQ(writer.CloseFragment(4, false),
              SharedRingBufferWriter::Outcome::kOk);

    // A second fragment is opened and never closed: it is abandoned.
    ASSERT_EQ(writer.OpenFragment(4, false).outcome,
              SharedRingBufferWriter::Outcome::kOk);
  }
  const DecodedChunk decoded = Decode(ring.get(), 0);
  EXPECT_EQ(decoded.state, ChunkState::kComplete);
  ASSERT_EQ(decoded.fragments.size(), 1u);
  EXPECT_EQ(decoded.fragments[0], "kept");
}

}  // namespace
}  // namespace perfetto::tracing_v2
