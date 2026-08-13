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

#include <string.h>
#include <algorithm>

#include <atomic>
#include <thread>
#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace tracing_v2 {

class SharedRingBufferTestPeer {
 public:
  static bool Reserve(SharedRingBuffer* ring, uint32_t* pos) {
    return ring->TryReserveWritePos(pos);
  }

  static SharedRingBuffer::Chunk Claim(SharedRingBuffer* ring,
                                       const ChunkHeader& header,
                                       uint32_t pos) {
    return ring->TryClaimReservedChunk(header, pos);
  }

  static uint32_t StateWord(SharedRingBuffer* ring, uint32_t pos) {
    return SharedRingBuffer::state_word(ring->chunk_at(pos))
        ->load(std::memory_order_acquire);
  }

  // Places both cursors near the 2^32 rollover, which a test cannot reach by
  // writing four billion chunks.
  static void SeedPositions(SharedRingBuffer* ring, uint32_t pos) {
    ring->ring_buffer_header()->write_pos.store(pos, std::memory_order_release);
    ring->ring_buffer_header()->read_pos.store(pos, std::memory_order_release);
    ring->read_pos_ = pos;
  }

  // Forges a header no writer of ours would emit, which is the only way to
  // reach the reader's "I cannot parse this" paths.
  static void SetControlBits(SharedRingBuffer* ring,
                             uint32_t pos,
                             uint8_t control_bits) {
    std::atomic<uint32_t>* const state =
        SharedRingBuffer::state_word(ring->chunk_at(pos));
    state->store(state->load(std::memory_order_acquire) | control_bits,
                 std::memory_order_release);
  }

  // Forges the whole state word, for reader paths our writers cannot reach,
  // e.g. a fragment count the payload cannot hold.
  static void SetStateWord(SharedRingBuffer* ring,
                           uint32_t pos,
                           uint32_t word) {
    SharedRingBuffer::state_word(ring->chunk_at(pos))
        ->store(word, std::memory_order_release);
  }

  // The step that validates a reader's copy: freeing the exact state the copy
  // was taken from. Lets a test stand in for a reader that copied |state| and
  // only got here after the writer had moved on.
  static bool TryFreeFromState(SharedRingBuffer* ring,
                               uint32_t pos,
                               uint32_t state) {
    return SharedRingBuffer::state_word(ring->chunk_at(pos))
        ->compare_exchange_strong(state, kFreeStateWord,
                                  std::memory_order_acq_rel,
                                  std::memory_order_acquire);
  }
};

namespace {

constexpr size_t kUint32RecordSize = kFragmentHeaderSize + sizeof(uint32_t);

ChunkHeader MakeHeader(WriterID writer_id, BufferID target_buffer) {
  ChunkHeader header;
  header.writer_id = writer_id;
  header.target_buffer = target_buffer;
  return header;
}

void AppendOneByteRecord(SharedRingBuffer::Chunk* chunk, uint8_t value) {
  uint8_t* const record = chunk->payload_begin() + chunk->payload_used();
  record[0] = 1;
  record[1] = value;
  chunk->AddFragment(chunk->payload_used() + 2);
}

void AppendUint32Record(SharedRingBuffer::Chunk* chunk, uint32_t value) {
  uint8_t* const record = chunk->payload_begin() + chunk->payload_used();
  record[0] = static_cast<uint8_t>(sizeof(uint32_t));
  record[1] = static_cast<uint8_t>(value);
  record[2] = static_cast<uint8_t>(value >> 8);
  record[3] = static_cast<uint8_t>(value >> 16);
  record[4] = static_cast<uint8_t>(value >> 24);
  chunk->AddFragment(chunk->payload_used() +
                     static_cast<uint32_t>(kUint32RecordSize));
}

void ReadUint32Records(const ChunkHeader& header,
                       const uint8_t* payload,
                       std::vector<uint32_t>* out) {
  size_t offset = 0;
  for (uint32_t record = 0; record < header.num_fragments; ++record) {
    PERFETTO_CHECK(payload[offset] == sizeof(uint32_t));
    out->push_back(static_cast<uint32_t>(payload[offset + 1]) |
                   (static_cast<uint32_t>(payload[offset + 2]) << 8) |
                   (static_cast<uint32_t>(payload[offset + 3]) << 16) |
                   (static_cast<uint32_t>(payload[offset + 4]) << 24));
    offset += kUint32RecordSize;
  }
}

SharedRingBuffer::ReadResult ReadPastSkippedChunks(SharedRingBuffer* ring,
                                                   ChunkHeader* header,
                                                   uint8_t* payload,
                                                   uint32_t* payload_size) {
  for (;;) {
    const SharedRingBuffer::ReadResult result =
        ring->TryReadChunk(header, payload, payload_size);
    if (result != SharedRingBuffer::ReadResult::kChunkSkipped)
      return result;
  }
}

TEST(SharedRingBufferV2Test, HeaderMatchesTheDocumentedSixByteLayout) {
  ChunkHeader header;
  header.writer_id = 0x1234;
  header.target_buffer = 0x5678;
  header.num_fragments = 0xab;
  header.flags =
      static_cast<uint8_t>(kFlagContinuesOnNextChunk | kFlagDataLoss);
  const uint32_t word = header.ToStateWord();

  EXPECT_EQ(static_cast<uint8_t>(word),
            static_cast<uint8_t>(kFlagContinuesOnNextChunk | kFlagDataLoss));
  EXPECT_EQ((word >> 8) & 0xff, 0xabu);
  EXPECT_EQ(word >> 16, 0x1234u);

  const ChunkHeader decoded = ChunkHeader::FromStateWord(word, 0x5678);
  EXPECT_EQ(decoded.writer_id, 0x1234);
  EXPECT_EQ(decoded.target_buffer, 0x5678);
  EXPECT_EQ(decoded.num_fragments, 0xab);
  EXPECT_EQ(decoded.version, kChunkVersion);
  EXPECT_FALSE(decoded.extended_header);
  EXPECT_EQ(decoded.flags,
            static_cast<uint8_t>(kFlagContinuesOnNextChunk | kFlagDataLoss));

  // The count field's boundary values pack exactly: empty, first commit, and
  // the kMaxChunkFragments ceiling that forces a writer onto a new chunk.
  for (uint32_t count : {0u, 1u, kMaxChunkFragments}) {
    ChunkHeader boundary;
    boundary.writer_id = 1;
    boundary.num_fragments = static_cast<uint8_t>(count);
    EXPECT_EQ((boundary.ToStateWord() >> 8) & 0xff, count);
    EXPECT_EQ(
        ChunkHeader::FromStateWord(boundary.ToStateWord(), 0).num_fragments,
        count);
  }

  // The target buffer stays a whole 16-bit field, outside the state word.
  uint8_t chunk[kChunkSize]{};
  StoreChunkTargetBuffer(chunk, 0x5678);
  EXPECT_EQ(chunk[4], 0x78);
  EXPECT_EQ(chunk[5], 0x56);
  EXPECT_EQ(LoadChunkTargetBuffer(chunk), 0x5678);

  // Bits 0-4 are ABI and must not move independently of the header version.
  EXPECT_EQ(kFlagAcquiredForWriting, 1u << 0);
  EXPECT_EQ(kFlagContinuesOnNextChunk, 1u << 1);
  EXPECT_EQ(kFlagContinuesFromPrevChunk, 1u << 2);
  EXPECT_EQ(kFlagDataLoss, 1u << 3);
  EXPECT_EQ(kFlagNeedsRewrite, 1u << 4);
  EXPECT_EQ(kChunkFlagsMask, 0x1f);
  EXPECT_EQ(kChunkVersionMask, 0x60);
  EXPECT_EQ(kChunkExtendedHeaderMask, 0x80);
}

// Which base layout is present and whether extra header bytes follow are two
// separate questions, so the version field and the extension bit have to
// survive each other.
TEST(SharedRingBufferV2Test, ControlByteVersionAndExtensionAreIndependent) {
  ChunkHeader header;
  header.writer_id = 1;
  header.flags = kFlagDataLoss;
  header.version = 2;
  header.extended_header = true;

  const ChunkHeader decoded = ChunkHeader::FromStateWord(header.ToStateWord(),
                                                         /*target_buffer=*/0);
  EXPECT_EQ(decoded.version, 2);
  EXPECT_TRUE(decoded.extended_header);
  EXPECT_EQ(decoded.flags, kFlagDataLoss);

  header.extended_header = false;
  EXPECT_EQ(ChunkHeader::FromStateWord(header.ToStateWord(), 0).version, 2);
}

TEST(SharedRingBufferV2Test, PublishesAndReadsACompleteChunk) {
  SharedRingBuffer ring(4);
  SharedRingBuffer::Chunk chunk =
      ring.TryAcquireChunkForWriting(MakeHeader(7, 11));
  ASSERT_TRUE(chunk.is_valid());
  AppendOneByteRecord(&chunk, 0x42);
  ASSERT_TRUE(ring.ReleaseChunkAsComplete(&chunk, /*added_flags=*/0));

  // A v0 writer leaves the version bits and the extension bit clear.
  EXPECT_EQ(SharedRingBufferTestPeer::StateWord(&ring, 0) &
                (kChunkVersionMask | kChunkExtendedHeaderMask),
            0u);

  ChunkHeader header;
  uint8_t payload[kChunkPayloadSize]{};
  uint32_t payload_size = 0;
  ASSERT_EQ(ring.TryReadChunk(&header, payload, &payload_size),
            SharedRingBuffer::ReadResult::kChunkRead);
  EXPECT_EQ(header.writer_id, 7);
  EXPECT_EQ(header.target_buffer, 11);
  EXPECT_EQ(header.num_fragments, 1);
  EXPECT_EQ(payload_size, 2u);
  EXPECT_EQ(payload[0], 1);
  EXPECT_EQ(payload[1], 0x42);
  EXPECT_FALSE(ring.has_pending_data());
}

// The reader copies the committed prefix and then frees the exact state word
// it copied under. That only proves anything because appending changes the
// word: the committed-fragment count lives in it. Without the count as a
// generation, an acquire/append/release cycle would restore the word bit for
// bit and the reader would free - and so lose - the appended record.
TEST(SharedRingBufferV2Test, AppendChangesTheStateWordAStaleReaderWouldFree) {
  SharedRingBuffer ring(4);
  SharedRingBuffer::Chunk chunk =
      ring.TryAcquireChunkForWriting(MakeHeader(9, 3));
  ASSERT_TRUE(chunk.is_valid());
  AppendOneByteRecord(&chunk, 0xAB);
  ASSERT_TRUE(ring.ReleaseChunkAsComplete(&chunk, /*added_flags=*/0));

  // What a reader that started copying here would try to free later.
  const uint32_t observed = SharedRingBufferTestPeer::StateWord(&ring, 0);

  ASSERT_TRUE(ring.TryReacquireChunkForWriting(&chunk));
  AppendOneByteRecord(&chunk, 0xCD);
  ASSERT_TRUE(ring.ReleaseChunkAsComplete(&chunk, /*added_flags=*/0));

  EXPECT_NE(SharedRingBufferTestPeer::StateWord(&ring, 0), observed);
  EXPECT_FALSE(SharedRingBufferTestPeer::TryFreeFromState(&ring, 0, observed));

  // ... so the record the writer appended is still there to be read.
  ChunkHeader header;
  uint8_t payload[kChunkPayloadSize]{};
  uint32_t payload_size = 0;
  ASSERT_EQ(ring.TryReadChunk(&header, payload, &payload_size),
            SharedRingBuffer::ReadResult::kChunkRead);
  EXPECT_EQ(header.num_fragments, 2);
  EXPECT_EQ(payload_size, 4u);
  EXPECT_EQ(payload[1], 0xAB);
  EXPECT_EQ(payload[3], 0xCD);
}

// Bit 7 and the version bits both say "this is not the header you know". The
// reader has to drop the chunk without going looking for a payload whose
// offset it cannot know, and without blaming a newer producer on corruption.
TEST(SharedRingBufferV2Test, SkipsHeadersItCannotParse) {
  for (uint8_t control_bits : {kChunkExtendedHeaderMask,
                               static_cast<uint8_t>(1 << kChunkVersionShift)}) {
    SharedRingBuffer ring(2);
    SharedRingBuffer::Chunk chunk =
        ring.TryAcquireChunkForWriting(MakeHeader(5, 1));
    ASSERT_TRUE(chunk.is_valid());
    AppendOneByteRecord(&chunk, 0x11);
    ASSERT_TRUE(ring.ReleaseChunkAsComplete(&chunk, /*added_flags=*/0));
    SharedRingBufferTestPeer::SetControlBits(&ring, /*pos=*/0, control_bits);

    ChunkHeader header;
    uint8_t payload[kChunkPayloadSize];
    memset(payload, 0xee, sizeof(payload));
    uint32_t payload_size = 0xffffffff;
    EXPECT_EQ(ring.TryReadChunk(&header, payload, &payload_size),
              SharedRingBuffer::ReadResult::kChunkSkipped);
    EXPECT_EQ(ring.stats().chunks_unsupported.load(), 1u);
    EXPECT_EQ(ring.stats().malformed_chunks.load(), 0u);
    EXPECT_EQ(payload_size, 0u);
    EXPECT_EQ(payload[0], 0xee);  // Nothing was copied out of the chunk.
    EXPECT_FALSE(ring.has_pending_data());
  }
}

TEST(SharedRingBufferV2Test, NeverOverwritesWhenFull) {
  SharedRingBuffer ring(2);
  SharedRingBuffer::Chunk first =
      ring.TryAcquireChunkForWriting(MakeHeader(1, 1));
  SharedRingBuffer::Chunk second =
      ring.TryAcquireChunkForWriting(MakeHeader(2, 1));
  ASSERT_TRUE(first.is_valid());
  ASSERT_TRUE(second.is_valid());
  EXPECT_FALSE(ring.TryAcquireChunkForWriting(MakeHeader(3, 1)).is_valid());

  AppendOneByteRecord(&first, 1);
  AppendOneByteRecord(&second, 2);
  ASSERT_TRUE(ring.ReleaseChunkAsComplete(&first, /*added_flags=*/0));
  ASSERT_TRUE(ring.ReleaseChunkAsComplete(&second, /*added_flags=*/0));

  ChunkHeader header;
  uint8_t payload[kChunkPayloadSize]{};
  uint32_t payload_size = 0;
  EXPECT_EQ(ring.TryReadChunk(&header, payload, &payload_size),
            SharedRingBuffer::ReadResult::kChunkRead);
  EXPECT_EQ(ring.TryReadChunk(&header, payload, &payload_size),
            SharedRingBuffer::ReadResult::kChunkRead);
}

TEST(SharedRingBufferV2Test, DelayedWriterCannotPublishAfterReaderPassesIt) {
  // At zero and at the 2^32 boundary: invalidating the reservation and
  // rejecting the late claim must not depend on where the cursors sit.
  for (uint32_t seed : {0u, 0xffffffffu}) {
    SharedRingBuffer ring(2);
    SharedRingBufferTestPeer::SeedPositions(&ring, seed);
    uint32_t pos = 0;
    ASSERT_TRUE(SharedRingBufferTestPeer::Reserve(&ring, &pos));
    EXPECT_EQ(pos, seed);

    ChunkHeader header;
    uint8_t payload[kChunkPayloadSize]{};
    uint32_t payload_size = 0;
    EXPECT_EQ(ring.TryReadChunk(&header, payload, &payload_size),
              SharedRingBuffer::ReadResult::kChunkSkipped);

    SharedRingBuffer::Chunk stale =
        SharedRingBufferTestPeer::Claim(&ring, MakeHeader(1, 1), pos);
    EXPECT_FALSE(stale.is_valid());
    EXPECT_FALSE(ring.has_pending_data());
  }
}

// A reservation the reader passed unclaimed leaves its slot tombstoned. The
// tombstone must kill only that reservation, not the physical slot: the next
// writer whose reservation maps there frees it and claims the slot for its
// own position. Without that reclaim, every lap onto the slot burns a fresh
// reservation, the reader tombstones the resulting hole again, and the ring
// permanently loses one position per lap.
TEST(SharedRingBufferV2Test, ReclaimsAStaleTombstoneInsteadOfBurningEveryLap) {
  for (uint32_t seed : {0u, 0xffffffffu}) {
    SharedRingBuffer ring(4);
    SharedRingBufferTestPeer::SeedPositions(&ring, seed);

    // Seed the poison: a reservation whose writer never shows up to claim.
    uint32_t stale_pos = 0;
    ASSERT_TRUE(SharedRingBufferTestPeer::Reserve(&ring, &stale_pos));
    EXPECT_EQ(stale_pos, seed);
    for (uint32_t i = 0; i < 3; ++i) {
      SharedRingBuffer::Chunk chunk =
          ring.TryAcquireChunkForWriting(MakeHeader(1, 1));
      ASSERT_TRUE(chunk.is_valid());
      AppendOneByteRecord(&chunk, static_cast<uint8_t>(i));
      ASSERT_TRUE(ring.ReleaseChunkAsComplete(&chunk, /*added_flags=*/0));
    }
    ChunkHeader header;
    uint8_t payload[kChunkPayloadSize]{};
    uint32_t payload_size = 0;
    EXPECT_EQ(ring.TryReadChunk(&header, payload, &payload_size),
              SharedRingBuffer::ReadResult::kChunkSkipped);
    for (uint32_t i = 0; i < 3; ++i) {
      ASSERT_EQ(ring.TryReadChunk(&header, payload, &payload_size),
                SharedRingBuffer::ReadResult::kChunkRead);
    }
    ASSERT_EQ(SharedRingBufferTestPeer::StateWord(&ring, stale_pos),
              kInvalidatedChunkHeader);
    EXPECT_EQ(ring.stats().chunks_invalidated.load(), 1u);

    // From here on, every lap must have all four slots usable, and the seeding
    // invalidation must stay the only one.
    for (uint32_t lap = 0; lap < 8; ++lap) {
      SharedRingBuffer::Chunk chunks[4];
      for (auto& chunk : chunks) {
        chunk = ring.TryAcquireChunkForWriting(MakeHeader(1, 1));
        ASSERT_TRUE(chunk.is_valid()) << "seed " << seed << " lap " << lap;
      }
      for (auto& chunk : chunks) {
        AppendOneByteRecord(&chunk, static_cast<uint8_t>(lap));
        ASSERT_TRUE(ring.ReleaseChunkAsComplete(&chunk, /*added_flags=*/0));
      }
      for (uint32_t i = 0; i < 4; ++i) {
        ASSERT_EQ(ring.TryReadChunk(&header, payload, &payload_size),
                  SharedRingBuffer::ReadResult::kChunkRead);
        EXPECT_EQ(payload[1], static_cast<uint8_t>(lap));
      }
    }
    EXPECT_EQ(ring.stats().chunks_invalidated.load(), 1u);
  }
}

// The reclaim path must not hand the original late writer a way to publish:
// when it frees the tombstone and transiently claims the slot, the
// consumed-position check has to reject the claim and leave the slot free for
// the incarnation's rightful claimant. Exercises the equality edge of
// IsPositionAtOrAfter: the reader stands exactly one past the stale position.
TEST(SharedRingBufferV2Test, StaleWriterRetryCannotPublishAtAConsumedPosition) {
  for (uint32_t seed : {0u, 0xffffffffu}) {
    SharedRingBuffer ring(4);
    SharedRingBufferTestPeer::SeedPositions(&ring, seed);

    uint32_t stale_pos = 0;
    ASSERT_TRUE(SharedRingBufferTestPeer::Reserve(&ring, &stale_pos));
    for (uint32_t i = 0; i < 3; ++i) {
      SharedRingBuffer::Chunk chunk =
          ring.TryAcquireChunkForWriting(MakeHeader(1, 1));
      ASSERT_TRUE(chunk.is_valid());
      AppendOneByteRecord(&chunk, static_cast<uint8_t>(i));
      ASSERT_TRUE(ring.ReleaseChunkAsComplete(&chunk, /*added_flags=*/0));
    }
    ChunkHeader header;
    uint8_t payload[kChunkPayloadSize]{};
    uint32_t payload_size = 0;
    EXPECT_EQ(ring.TryReadChunk(&header, payload, &payload_size),
              SharedRingBuffer::ReadResult::kChunkSkipped);
    for (uint32_t i = 0; i < 3; ++i) {
      ASSERT_EQ(ring.TryReadChunk(&header, payload, &payload_size),
                SharedRingBuffer::ReadResult::kChunkRead);
    }

    // The live claimant reserves the slot's next incarnation first, then the
    // stale writer finally shows up.
    uint32_t live_pos = 0;
    ASSERT_TRUE(SharedRingBufferTestPeer::Reserve(&ring, &live_pos));
    EXPECT_EQ(live_pos, seed + 4);

    SharedRingBuffer::Chunk stale =
        SharedRingBufferTestPeer::Claim(&ring, MakeHeader(1, 1), stale_pos);
    EXPECT_FALSE(stale.is_valid());
    EXPECT_EQ(SharedRingBufferTestPeer::StateWord(&ring, stale_pos),
              kFreeStateWord);

    SharedRingBuffer::Chunk live =
        SharedRingBufferTestPeer::Claim(&ring, MakeHeader(2, 1), live_pos);
    ASSERT_TRUE(live.is_valid());
    AppendOneByteRecord(&live, 0x42);
    ASSERT_TRUE(ring.ReleaseChunkAsComplete(&live, /*added_flags=*/0));

    ASSERT_EQ(ring.TryReadChunk(&header, payload, &payload_size),
              SharedRingBuffer::ReadResult::kChunkRead);
    EXPECT_EQ(header.writer_id, 2);
    EXPECT_EQ(payload[1], 0x42);
    EXPECT_FALSE(ring.has_pending_data());
  }
}

TEST(SharedRingBufferV2Test, ActiveWriterRelocatesInsteadOfBlockingReader) {
  SharedRingBuffer ring(4);
  SharedRingBuffer::Chunk chunk =
      ring.TryAcquireChunkForWriting(MakeHeader(3, 9));
  ASSERT_TRUE(chunk.is_valid());
  AppendOneByteRecord(&chunk, 0x5a);

  ChunkHeader header;
  uint8_t payload[kChunkPayloadSize]{};
  uint32_t payload_size = 0;
  EXPECT_EQ(ring.TryReadChunk(&header, payload, &payload_size),
            SharedRingBuffer::ReadResult::kChunkSkipped);
  ASSERT_TRUE(ring.ReleaseChunkAsComplete(&chunk, /*added_flags=*/0));

  ASSERT_EQ(ReadPastSkippedChunks(&ring, &header, payload, &payload_size),
            SharedRingBuffer::ReadResult::kChunkRead);
  EXPECT_EQ(header.writer_id, 3);
  EXPECT_EQ(header.target_buffer, 9);
  EXPECT_EQ(payload[1], 0x5a);
  EXPECT_EQ(ring.stats().chunks_rewritten.load(), 1u);
}

// A relocated payload lands in a slot that a previous lap left dirty, and
// nothing wipes the bytes after it. What keeps those bytes out of the trace
// is the republished fragment count, so relocation has to carry over exactly
// the counted records and their local byte offset.
TEST(SharedRingBufferV2Test, RelocationIgnoresStaleBytesPastTheCountedRecords) {
  SharedRingBuffer ring(2);

  // Dirty both slots with record-shaped bytes, then hand them back.
  for (int i = 0; i < 2; ++i) {
    SharedRingBuffer::Chunk dirty =
        ring.TryAcquireChunkForWriting(MakeHeader(9, 1));
    ASSERT_TRUE(dirty.is_valid());
    while (dirty.payload_free() >= 2)
      AppendOneByteRecord(&dirty, 0x01);
    ASSERT_TRUE(ring.ReleaseChunkAsComplete(&dirty, /*added_flags=*/0));
  }
  ChunkHeader drained;
  uint8_t drained_payload[kChunkPayloadSize]{};
  uint32_t drained_size = 0;
  for (int i = 0; i < 2; ++i) {
    ASSERT_EQ(ring.TryReadChunk(&drained, drained_payload, &drained_size),
              SharedRingBuffer::ReadResult::kChunkRead);
  }

  // One short record, then let the reader step over the live writer.
  SharedRingBuffer::Chunk chunk =
      ring.TryAcquireChunkForWriting(MakeHeader(7, 1));
  ASSERT_TRUE(chunk.is_valid());
  AppendOneByteRecord(&chunk, 0x42);
  ChunkHeader skipped;
  uint8_t skipped_payload[kChunkPayloadSize]{};
  uint32_t skipped_size = 0;
  ASSERT_EQ(ring.TryReadChunk(&skipped, skipped_payload, &skipped_size),
            SharedRingBuffer::ReadResult::kChunkSkipped);

  // The release now has to relocate into one of the dirty slots.
  ASSERT_TRUE(ring.ReleaseChunkAsComplete(&chunk, /*added_flags=*/0));
  EXPECT_EQ(ring.stats().chunks_rewritten.load(), 1u);

  ChunkHeader header;
  uint8_t payload[kChunkPayloadSize]{};
  uint32_t payload_size = 0;
  ASSERT_EQ(ring.TryReadChunk(&header, payload, &payload_size),
            SharedRingBuffer::ReadResult::kChunkRead);
  EXPECT_EQ(header.writer_id, 7);
  EXPECT_EQ(header.num_fragments, 1);  // One record, one byte of data...
  EXPECT_EQ(payload_size, 2u);
  EXPECT_EQ(payload[0], 1);
  EXPECT_EQ(payload[1], 0x42);
}

// The cursor contract in one place: positions are wrapping tickets ordered
// by a half-space comparison, valid only within 2^31 advances. Every other
// test exercises the comparison through ring operations; this one pins its
// raw edges, including the one deliberately outside the contract.
TEST(SharedRingBufferV2Test, PositionComparisonCoversTheWrapBoundary) {
  // Equality, away from and at the wrap.
  EXPECT_TRUE(SharedRingBuffer::IsPositionAtOrAfter(0, 0));
  EXPECT_TRUE(SharedRingBuffer::IsPositionAtOrAfter(0xffffffffu, 0xffffffffu));

  // One advance apart, across zero: 0 comes after 0xffffffff.
  EXPECT_TRUE(SharedRingBuffer::IsPositionAtOrAfter(0, 0xffffffffu));
  EXPECT_FALSE(SharedRingBuffer::IsPositionAtOrAfter(0xffffffffu, 0));

  // The largest distance the contract covers: 2^31 - 1 advances.
  EXPECT_TRUE(SharedRingBuffer::IsPositionAtOrAfter(0x7fffffffu, 0));
  EXPECT_TRUE(SharedRingBuffer::IsPositionAtOrAfter(0x7ffffffeu, 0xffffffffu));

  // Exactly 2^31 advances is OUTSIDE the contract: the half-space test calls
  // it "before" from both viewpoints at once. Nothing may rely on this
  // value; the ring keeps live positions at most num_chunks (< 2^31) apart,
  // and the asserts document the cliff rather than a behavior to depend on.
  EXPECT_FALSE(SharedRingBuffer::IsPositionAtOrAfter(0x80000000u, 0));
  EXPECT_FALSE(SharedRingBuffer::IsPositionAtOrAfter(0, 0x80000000u));
}

// Capacity is the unsigned difference between two cursors that roll over at
// 2^32, and reservation compares them on every attempt. Nothing may go wrong
// at the rollover itself, and no snapshot older than the sampled reader
// position may be carried into that comparison.
TEST(SharedRingBufferV2Test, ReservesAcrossTheCursorRollover) {
  SharedRingBuffer ring(4);
  SharedRingBufferTestPeer::SeedPositions(&ring, 0xfffffffe);

  ChunkHeader header;
  uint8_t payload[kChunkPayloadSize]{};
  uint32_t payload_size = 0;
  for (uint32_t i = 0; i < 8; ++i) {
    SharedRingBuffer::Chunk chunk =
        ring.TryAcquireChunkForWriting(MakeHeader(1, 2));
    ASSERT_TRUE(chunk.is_valid()) << "iteration " << i;
    AppendOneByteRecord(&chunk, static_cast<uint8_t>(i));
    ASSERT_TRUE(ring.ReleaseChunkAsComplete(&chunk, /*added_flags=*/0));
    ASSERT_EQ(ring.TryReadChunk(&header, payload, &payload_size),
              SharedRingBuffer::ReadResult::kChunkRead);
    EXPECT_EQ(payload[1], static_cast<uint8_t>(i));
  }
}

// A full ring at the rollover must still report full, an empty one must still
// report empty, and both must recover as soon as the cursors move.
TEST(SharedRingBufferV2Test, ReportsFullAtTheCursorRollover) {
  SharedRingBuffer ring(2);
  SharedRingBufferTestPeer::SeedPositions(&ring, 0xffffffff);

  ChunkHeader header;
  uint8_t payload[kChunkPayloadSize]{};
  uint32_t payload_size = 0;
  // Empty at the boundary: equal cursors mean no data, not a full lap.
  EXPECT_EQ(ring.TryReadChunk(&header, payload, &payload_size),
            SharedRingBuffer::ReadResult::kNoData);

  SharedRingBuffer::Chunk first =
      ring.TryAcquireChunkForWriting(MakeHeader(1, 1));
  SharedRingBuffer::Chunk second =
      ring.TryAcquireChunkForWriting(MakeHeader(2, 1));
  ASSERT_TRUE(first.is_valid() && second.is_valid());
  EXPECT_FALSE(ring.TryAcquireChunkForWriting(MakeHeader(3, 1)).is_valid());

  AppendOneByteRecord(&first, 0x11);
  ASSERT_TRUE(ring.ReleaseChunkAsComplete(&first, /*added_flags=*/0));
  ASSERT_EQ(ring.TryReadChunk(&header, payload, &payload_size),
            SharedRingBuffer::ReadResult::kChunkRead);

  SharedRingBuffer::Chunk third =
      ring.TryAcquireChunkForWriting(MakeHeader(3, 1));
  EXPECT_TRUE(third.is_valid());

  AppendOneByteRecord(&second, 0x22);
  ASSERT_TRUE(ring.ReleaseChunkAsComplete(&second, /*added_flags=*/0));
  AppendOneByteRecord(&third, 0x33);
  ASSERT_TRUE(ring.ReleaseChunkAsComplete(&third, /*added_flags=*/0));
}

TEST(SharedRingBufferV2Test, WrapsPositionsWithoutLosingCapacity) {
  // From zero, and seeded so the run crosses 2^32 mid-way: several complete
  // laps on each side of the boundary with every slot staying usable.
  for (uint32_t seed : {0u, 0xffffffffu - 500u}) {
    SharedRingBuffer ring(4);
    SharedRingBufferTestPeer::SeedPositions(&ring, seed);
    ChunkHeader header;
    uint8_t payload[kChunkPayloadSize]{};
    uint32_t payload_size = 0;
    for (uint32_t i = 0; i < 1000; ++i) {
      SharedRingBuffer::Chunk chunk =
          ring.TryAcquireChunkForWriting(MakeHeader(1, 2));
      ASSERT_TRUE(chunk.is_valid()) << "seed " << seed << " iteration " << i;
      AppendOneByteRecord(&chunk, static_cast<uint8_t>(i));
      ASSERT_TRUE(ring.ReleaseChunkAsComplete(&chunk, /*added_flags=*/0));
      ASSERT_EQ(ring.TryReadChunk(&header, payload, &payload_size),
                SharedRingBuffer::ReadResult::kChunkRead);
      EXPECT_EQ(payload[1], static_cast<uint8_t>(i));
    }
  }
}

// Every other record is appended to a chunk taken back from the ring, so the
// reader regularly copies a prefix while its writer is adding to the same
// chunk. Under TSAN this is also what shows the two only ever touch disjoint
// byte ranges. |seed_pos| places the cursors before the threads start, so the
// same interleavings can be run right below the 2^32 rollover.
// |forced_stale_claims| adds a laggard that repeatedly reserves a position,
// lets the reader pass it, and only then claims: each cycle plants a
// tombstone in front of the running writers, who must reclaim the slot.
void RunConcurrentWritersAndReader(SharedRingBuffer* ring,
                                   uint32_t seed_pos,
                                   uint32_t forced_stale_claims = 0) {
  constexpr uint32_t kNumWriters = 4;
  constexpr uint32_t kRecordsPerWriter = 2000;
  SharedRingBufferTestPeer::SeedPositions(ring, seed_pos);
  std::atomic<bool> start{false};
  std::atomic<uint32_t> writers_done{0};
  std::atomic<bool> laggard_done{forced_stale_claims == 0};
  std::vector<std::thread> writers;

  for (uint32_t writer = 0; writer < kNumWriters; ++writer) {
    writers.emplace_back([writer, ring, &start, &writers_done] {
      const ChunkHeader header =
          MakeHeader(static_cast<WriterID>(writer + 1), 1);
      SharedRingBuffer::Chunk chunk;
      while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();

      for (uint32_t record = 0; record < kRecordsPerWriter; ++record) {
        const uint32_t value = (writer << 24) | record;
        if ((record & 1) != 0 && chunk.is_valid() &&
            chunk.payload_free() >= kUint32RecordSize &&
            ring->TryReacquireChunkForWriting(&chunk)) {
          AppendUint32Record(&chunk, value);
          if (ring->ReleaseChunkAsComplete(&chunk, /*added_flags=*/0))
            continue;
        }
        for (;;) {
          chunk = ring->TryAcquireChunkForWriting(header);
          if (!chunk.is_valid()) {
            std::this_thread::yield();
            continue;
          }
          AppendUint32Record(&chunk, value);
          if (ring->ReleaseChunkAsComplete(&chunk, /*added_flags=*/0))
            break;
        }
        if ((record & 7) == 0)
          std::this_thread::yield();
      }
      writers_done.fetch_add(1, std::memory_order_release);
    });
  }

  std::thread laggard;
  if (forced_stale_claims != 0) {
    laggard = std::thread([ring, forced_stale_claims, &start, &laggard_done] {
      while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();
      for (uint32_t i = 0; i < forced_stale_claims; ++i) {
        uint32_t pos = 0;
        while (!SharedRingBufferTestPeer::Reserve(ring, &pos))
          std::this_thread::yield();
        // Claim only once the reader has provably consumed the reservation,
        // so the claim below is always the delayed-writer case.
        while (
            !SharedRingBuffer::IsPositionAtOrAfter(ring->read_pos(), pos + 1)) {
          std::this_thread::yield();
        }
        SharedRingBuffer::Chunk stale = SharedRingBufferTestPeer::Claim(
            ring, MakeHeader(kNumWriters + 1, 1), pos);
        EXPECT_FALSE(stale.is_valid()) << "stale claim " << i;
      }
      laggard_done.store(true, std::memory_order_release);
    });
  }

  std::vector<uint32_t> observed;
  observed.reserve(kNumWriters * kRecordsPerWriter);
  start.store(true, std::memory_order_release);
  // The reader must outlive the laggard: its last reservation is consumed
  // here, and its wait for read_pos would deadlock against an exited reader.
  while (writers_done.load(std::memory_order_acquire) != kNumWriters ||
         !laggard_done.load(std::memory_order_acquire) ||
         ring->has_pending_data()) {
    ChunkHeader header;
    uint8_t payload[kChunkPayloadSize]{};
    uint32_t payload_size = 0;
    const SharedRingBuffer::ReadResult result =
        ring->TryReadChunk(&header, payload, &payload_size);
    if (result == SharedRingBuffer::ReadResult::kChunkRead) {
      ReadUint32Records(header, payload, &observed);
    } else if (result == SharedRingBuffer::ReadResult::kNoData) {
      std::this_thread::yield();
    }
  }
  for (std::thread& writer : writers)
    writer.join();
  if (laggard.joinable())
    laggard.join();

  std::vector<uint32_t> expected;
  expected.reserve(kNumWriters * kRecordsPerWriter);
  for (uint32_t writer = 0; writer < kNumWriters; ++writer) {
    for (uint32_t record = 0; record < kRecordsPerWriter; ++record)
      expected.push_back((writer << 24) | record);
  }
  std::sort(observed.begin(), observed.end());
  EXPECT_EQ(observed, expected);
}

TEST(SharedRingBufferV2Test, ConcurrentWritersAndReaderPreserveEveryRecord) {
  SharedRingBuffer ring(64);
  RunConcurrentWritersAndReader(&ring, /*seed_pos=*/0);
}

TEST(SharedRingBufferV2Test,
     ConcurrentWritersAndReaderPreserveEveryRecordAcrossRollover) {
  // Far enough below 2^32 that reservations start before the boundary, close
  // enough that the run's several thousand positions must cross it.
  SharedRingBuffer ring(64);
  RunConcurrentWritersAndReader(&ring, /*seed_pos=*/0xffffffffu - 64u);
}

// Tombstone reclaim under real concurrency: a laggard keeps planting stale
// tombstones while four writers lap the ring, so writers keep hitting slots
// that hold a dead reservation's tombstone. Record conservation proves
// reclaim never loses or duplicates data; the invalidation bound is the
// liveness half. This workload has a background invalidation rate of well
// under one per lap (holes burned against slots a stepped-over or transient
// writer still occupies), while a single unreclaimed tombstone costs one
// position on EVERY later lap - sixteen of them would push the counter to
// many multiples of the lap count.
TEST(SharedRingBufferV2Test, ManyWritersReclaimForcedStaleTombstones) {
  constexpr uint32_t kForcedStaleClaims = 16;
  SharedRingBuffer ring(64);
  RunConcurrentWritersAndReader(&ring, /*seed_pos=*/0, kForcedStaleClaims);

  const uint32_t laps = ring.write_pos() / ring.num_chunks();
  EXPECT_LT(ring.stats().chunks_invalidated.load(), 2u * laps);
}

// The reader copies only the bytes the counted records span, so record-shaped
// garbage past the committed prefix - whatever an earlier lap or an unfinished
// append left there - never reaches the output buffer.
TEST(SharedRingBufferV2Test, CopiesOnlyTheCountedRecords) {
  SharedRingBuffer ring(4);
  SharedRingBuffer::Chunk chunk =
      ring.TryAcquireChunkForWriting(MakeHeader(3, 1));
  ASSERT_TRUE(chunk.is_valid());
  AppendOneByteRecord(&chunk, 0x42);
  memset(chunk.payload_begin() + chunk.payload_used(), 0x01,
         kChunkPayloadSize - chunk.payload_used());
  ASSERT_TRUE(ring.ReleaseChunkAsComplete(&chunk, /*added_flags=*/0));

  ChunkHeader header;
  uint8_t payload[kChunkPayloadSize];
  memset(payload, 0xee, sizeof(payload));
  uint32_t payload_size = 0;
  ASSERT_EQ(ring.TryReadChunk(&header, payload, &payload_size),
            SharedRingBuffer::ReadResult::kChunkRead);
  EXPECT_EQ(header.num_fragments, 1);
  EXPECT_EQ(payload_size, 2u);
  EXPECT_EQ(payload[0], 1);
  EXPECT_EQ(payload[1], 0x42);
  EXPECT_EQ(payload[2], 0xee);  // Nothing past the counted records came over.
}

// A record whose size byte reaches past the payload capacity cannot come from
// TraceWriterV2 - FinalizeFragment() bounds every size it writes - so it is
// payload garbage. The reader must copy nothing, count it, free the slot and
// keep the ring flowing.
TEST(SharedRingBufferV2Test, RejectsARecordReachingPastTheCapacity) {
  SharedRingBuffer ring(4);
  SharedRingBuffer::Chunk chunk =
      ring.TryAcquireChunkForWriting(MakeHeader(5, 1));
  ASSERT_TRUE(chunk.is_valid());
  chunk.payload_begin()[0] = 0xff;  // Claims more bytes than can follow it.
  chunk.AddFragment(2);
  ASSERT_TRUE(ring.ReleaseChunkAsComplete(&chunk, /*added_flags=*/0));

  ChunkHeader header;
  uint8_t payload[kChunkPayloadSize];
  memset(payload, 0xee, sizeof(payload));
  uint32_t payload_size = 0;
  EXPECT_EQ(ring.TryReadChunk(&header, payload, &payload_size),
            SharedRingBuffer::ReadResult::kChunkSkipped);
  EXPECT_EQ(ring.stats().malformed_chunks.load(), 1u);
  EXPECT_EQ(payload_size, 0u);
  EXPECT_EQ(payload[0], 0xee);  // Nothing was copied out of the chunk.

  // The slot was freed: the ring keeps flowing.
  SharedRingBuffer::Chunk next =
      ring.TryAcquireChunkForWriting(MakeHeader(5, 1));
  ASSERT_TRUE(next.is_valid());
  AppendOneByteRecord(&next, 0x11);
  ASSERT_TRUE(ring.ReleaseChunkAsComplete(&next, /*added_flags=*/0));
  ASSERT_EQ(ring.TryReadChunk(&header, payload, &payload_size),
            SharedRingBuffer::ReadResult::kChunkRead);
  EXPECT_EQ(payload[1], 0x11);
}

// Records that fit individually can still lie collectively: their cumulative
// span must stay inside the payload for every prefix of the walk.
TEST(SharedRingBufferV2Test, RejectsRecordsOverflowingCumulatively) {
  SharedRingBuffer ring(4);
  SharedRingBuffer::Chunk chunk =
      ring.TryAcquireChunkForWriting(MakeHeader(6, 1));
  ASSERT_TRUE(chunk.is_valid());
  chunk.payload_begin()[0] = 200;  // A 201-byte record: fits alone.
  chunk.AddFragment(201);
  chunk.payload_begin()[201] = 200;  // A second one cannot also fit.
  chunk.AddFragment(203);
  ASSERT_TRUE(ring.ReleaseChunkAsComplete(&chunk, /*added_flags=*/0));

  ChunkHeader header;
  uint8_t payload[kChunkPayloadSize]{};
  uint32_t payload_size = 0;
  EXPECT_EQ(ring.TryReadChunk(&header, payload, &payload_size),
            SharedRingBuffer::ReadResult::kChunkSkipped);
  EXPECT_EQ(ring.stats().malformed_chunks.load(), 1u);
  EXPECT_EQ(payload_size, 0u);
}

// A fragment count no payload could hold fails the walk by arithmetic: even
// all-empty records need one byte each. Our writers cannot produce it, so it
// is forged through the test peer.
TEST(SharedRingBufferV2Test, RejectsACountThePayloadCannotHold) {
  SharedRingBuffer ring(2);
  SharedRingBuffer::Chunk chunk =
      ring.TryAcquireChunkForWriting(MakeHeader(7, 1));
  ASSERT_TRUE(chunk.is_valid());
  AppendOneByteRecord(&chunk, 0x42);
  ASSERT_TRUE(ring.ReleaseChunkAsComplete(&chunk, /*added_flags=*/0));
  // Keep the control byte and the writer id; forge the count to the ceiling.
  const uint32_t forged =
      (SharedRingBufferTestPeer::StateWord(&ring, 0) & 0xffff00ffu) |
      (kMaxChunkFragments << 8);
  SharedRingBufferTestPeer::SetStateWord(&ring, 0, forged);

  ChunkHeader header;
  uint8_t payload[kChunkPayloadSize]{};
  uint32_t payload_size = 0;
  EXPECT_EQ(ring.TryReadChunk(&header, payload, &payload_size),
            SharedRingBuffer::ReadResult::kChunkSkipped);
  EXPECT_EQ(ring.stats().malformed_chunks.load(), 1u);
  EXPECT_FALSE(ring.has_pending_data());
}

// An empty complete chunk has no committed fragment to vouch for its
// target-buffer bytes, so the reader must not surface them as a destination,
// initialized or not.
TEST(SharedRingBufferV2Test, DoesNotReadTheTargetBufferOfAZeroFragmentChunk) {
  SharedRingBuffer ring(2);
  SharedRingBuffer::Chunk chunk =
      ring.TryAcquireChunkForWriting(MakeHeader(9, 0xdead));
  ASSERT_TRUE(chunk.is_valid());
  ASSERT_TRUE(ring.ReleaseChunkAsComplete(&chunk, /*added_flags=*/0));

  ChunkHeader header;
  uint8_t payload[kChunkPayloadSize]{};
  uint32_t payload_size = 0;
  ASSERT_EQ(ring.TryReadChunk(&header, payload, &payload_size),
            SharedRingBuffer::ReadResult::kChunkRead);
  EXPECT_EQ(header.writer_id, 9);
  EXPECT_EQ(header.num_fragments, 0);
  EXPECT_EQ(payload_size, 0u);
  EXPECT_EQ(header.target_buffer, 0);  // 0xdead was stored, and not read.
}

// kChunkPayloadSize empty records are the densest legal record list at this
// chunk size: the byte capacity binds before the eight-bit count can, which
// is exactly what the capacity static_assert in the ABI header freezes.
TEST(SharedRingBufferV2Test, ReadsAChunkPackedWithEmptyFragments) {
  SharedRingBuffer ring(2);
  SharedRingBuffer::Chunk chunk =
      ring.TryAcquireChunkForWriting(MakeHeader(4, 2));
  ASSERT_TRUE(chunk.is_valid());
  for (uint32_t i = 0; i < kChunkPayloadSize; ++i) {
    chunk.payload_begin()[i] = 0;  // A [size=0] record: an empty fragment.
    chunk.AddFragment(i + 1);
  }
  EXPECT_EQ(chunk.num_fragments(), kChunkPayloadSize);
  EXPECT_EQ(chunk.payload_free(), 0u);
  ASSERT_TRUE(ring.ReleaseChunkAsComplete(&chunk, /*added_flags=*/0));

  ChunkHeader header;
  uint8_t payload[kChunkPayloadSize]{};
  uint32_t payload_size = 0;
  ASSERT_EQ(ring.TryReadChunk(&header, payload, &payload_size),
            SharedRingBuffer::ReadResult::kChunkRead);
  EXPECT_EQ(header.num_fragments, kChunkPayloadSize);
  EXPECT_EQ(payload_size, kChunkPayloadSize);
}

}  // namespace
}  // namespace tracing_v2
}  // namespace perfetto
