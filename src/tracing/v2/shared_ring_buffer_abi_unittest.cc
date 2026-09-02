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

#include "src/tracing/v2/shared_ring_buffer_abi.h"

#include <stdint.h>

#include <vector>

#include "src/tracing/v2/shared_ring_buffer_test_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::tracing_v2 {
namespace {

using test::WrapCountOf;

// Chunk state word
// ----------------

// Keep exact wire values in the test, rather than obscuring the ABI header with
// compile-time examples. These literals are intentionally independent of the
// masks used by the encoding helpers.
TEST(SharedRingBufferABITest, StateWordEncoding) {
  EXPECT_EQ(MakeFreeStateWord(0), 0x00000000u);
  EXPECT_EQ(MakeFreeStateWord(5), 0x00050000u);
  EXPECT_EQ(MakeFreeStateWord(0xffff), 0xffff0000u);
  EXPECT_EQ(MakeDataStateWord(ChunkState::kBeingWritten,
                              ChunkFormat::kTargetBuffer, 0, 0, 7),
            0x00070001u);
  EXPECT_EQ(MakeDataStateWord(ChunkState::kComplete, ChunkFormat::kTargetBuffer,
                              0, 3, 7),
            0x00070302u);
  EXPECT_EQ(MakeDataStateWord(ChunkState::kRewriteRequested,
                              ChunkFormat::kTargetBuffer, 0, 3, 7),
            0x00070303u);
  EXPECT_EQ(MakeDataStateWord(ChunkState::kComplete, ChunkFormat::kTargetBuffer,
                              kFlagContinuesOnNextChunk, 2, 0x1234),
            0x12340242u);
  EXPECT_EQ(kRewriteAcknowledgedStateWord, 0x00000004u);
}

TEST(SharedRingBufferABITest, PayloadFlagEncoding) {
  EXPECT_EQ(kFlagDataLoss, 0x00000020u);
  EXPECT_EQ(kFlagContinuesOnNextChunk, 0x00000040u);
  EXPECT_EQ(kFlagContinuesFromPrevChunk, 0x00000080u);
}

TEST(SharedRingBufferABITest, StateDecoding) {
  EXPECT_EQ(ChunkStateOf(0x00000000u), ChunkState::kFree);
  EXPECT_EQ(ChunkStateOf(0x00000001u), ChunkState::kBeingWritten);
  EXPECT_EQ(ChunkStateOf(0x00000002u), ChunkState::kComplete);
  EXPECT_EQ(ChunkStateOf(0x00000003u), ChunkState::kRewriteRequested);
  EXPECT_EQ(ChunkStateOf(0x00000004u), ChunkState::kRewriteAcknowledged);
  EXPECT_EQ(ChunkStateOf(0x00000005u), ChunkState::kReserved5);
  EXPECT_EQ(ChunkStateOf(0x00000006u), ChunkState::kReserved6);
  EXPECT_EQ(ChunkStateOf(0x00000007u), ChunkState::kReserved7);

  EXPECT_TRUE(HasDataFields(ChunkState::kBeingWritten));
  EXPECT_TRUE(HasDataFields(ChunkState::kComplete));
  EXPECT_TRUE(HasDataFields(ChunkState::kRewriteRequested));
  EXPECT_FALSE(HasDataFields(ChunkState::kFree));
  EXPECT_FALSE(HasDataFields(ChunkState::kRewriteAcknowledged));
  EXPECT_FALSE(HasDataFields(ChunkState::kReserved5));
  EXPECT_FALSE(HasDataFields(ChunkState::kReserved6));
  EXPECT_FALSE(HasDataFields(ChunkState::kReserved7));
}

TEST(SharedRingBufferABITest, DataStateFieldsRoundTrip) {
  const ChunkState kStates[] = {ChunkState::kBeingWritten,
                                ChunkState::kComplete,
                                ChunkState::kRewriteRequested};
  const uint32_t kFlagSets[] = {
      0,
      kFlagContinuesFromPrevChunk,
      kFlagContinuesOnNextChunk,
      kFlagDataLoss,
      kFlagContinuesFromPrevChunk | kFlagDataLoss,
      kFlagContinuesFromPrevChunk | kFlagContinuesOnNextChunk | kFlagDataLoss};
  const uint32_t kCounts[] = {0, 1, 127, 128, 254, 255};
  const WriterID kWriters[] = {0, 1, 0x1234, 0x7fff, 0xffff};

  for (ChunkState state : kStates) {
    for (uint32_t flags : kFlagSets) {
      for (uint32_t count : kCounts) {
        for (WriterID writer : kWriters) {
          const uint32_t word = MakeDataStateWord(
              state, ChunkFormat::kTargetBuffer, flags, count, writer);
          EXPECT_EQ(ChunkStateOf(word), state);
          EXPECT_EQ(ChunkFormatOf(word), ChunkFormat::kTargetBuffer);
          EXPECT_EQ(PayloadFlagsOf(word), flags);
          EXPECT_EQ(NumFragmentsOf(word), count);
          EXPECT_EQ(WriterIDOf(word), writer);
        }
      }
    }
  }
}

TEST(SharedRingBufferABITest, FormatRoundTrip) {
  const ChunkFormat kFormats[] = {
      ChunkFormat::kTargetBuffer, ChunkFormat::kReservedRouting,
      ChunkFormat::kReserved2, ChunkFormat::kReserved3};
  for (ChunkFormat format : kFormats) {
    const uint32_t word = MakeDataStateWord(ChunkState::kComplete, format,
                                            kFlagDataLoss, 9, 0xabcd);
    EXPECT_EQ(ChunkFormatOf(word), format);
    // Whatever the format is, the state and the fields the reader needs to
    // arbitrate ownership stay where they are. That is what lets an old reader
    // release a chunk whose layout it has never heard of.
    EXPECT_EQ(ChunkStateOf(word), ChunkState::kComplete);
    EXPECT_EQ(NumFragmentsOf(word), 9u);
    EXPECT_EQ(WriterIDOf(word), 0xabcd);
  }
}

TEST(SharedRingBufferABITest, ReplaceChunkState) {
  // Only the state bits change.
  const uint32_t being_written = MakeDataStateWord(
      ChunkState::kBeingWritten, ChunkFormat::kReservedRouting,
      kFlagContinuesFromPrevChunk | kFlagDataLoss, 17, 0x0f0f);
  const uint32_t marked =
      ReplaceChunkState(being_written, ChunkState::kRewriteRequested);

  EXPECT_EQ(ChunkStateOf(marked), ChunkState::kRewriteRequested);
  EXPECT_EQ(being_written & ~kChunkStateMask, marked & ~kChunkStateMask);
  EXPECT_EQ(ChunkFormatOf(marked), ChunkFormat::kReservedRouting);
  EXPECT_EQ(PayloadFlagsOf(marked),
            kFlagContinuesFromPrevChunk | kFlagDataLoss);
  EXPECT_EQ(NumFragmentsOf(marked), 17u);
  EXPECT_EQ(WriterIDOf(marked), 0x0f0f);
}

TEST(SharedRingBufferABITest, FreeWordLayout) {
  EXPECT_EQ(kWrapCountShift, 16u);

  EXPECT_EQ(WrapCountOf(MakeFreeStateWord(0)), 0u);
  EXPECT_EQ(WrapCountOf(MakeFreeStateWord(1)), 1u);
  EXPECT_EQ(WrapCountOf(MakeFreeStateWord(0xffff)), 0xffffu);

  // MakeFreeStateWord takes a uint16_t, so no caller can produce a word with a
  // reserved bit set; decoding one still yields only bytes 2-3.
  EXPECT_EQ(WrapCountOf(0x0005fff8u), 5u);
  EXPECT_EQ(ChunkStateOf(0x0005fff8u), ChunkState::kFree);
}

// Packed read/write positions
// ---------------------------

TEST(SharedRingBufferABITest, RwPositionsEncoding) {
  // write_pos occupies the high half of the numeric value and read_pos the low
  // half.
  EXPECT_EQ(PackRwPositions(0, 0), 0u);
  EXPECT_EQ(PackRwPositions(1, 0), 0x0000000100000000ull);
  EXPECT_EQ(PackRwPositions(0, 1), 0x0000000000000001ull);
  EXPECT_EQ(PackRwPositions(0xdeadbeefu, 0x12345678u), 0xdeadbeef12345678ull);
  EXPECT_EQ(PackRwPositions(0x00000002u, 0xfffffffcu), 0x00000002fffffffcull);
}

TEST(SharedRingBufferABITest, RwPositionsRoundTrip) {
  const uint32_t kPositions[] = {0u,          1u,          0x7fffffffu,
                                 0x80000000u, 0xfffffffeu, 0xffffffffu};
  for (uint32_t write_pos : kPositions) {
    for (uint32_t read_pos : kPositions) {
      const uint64_t rw_positions = PackRwPositions(write_pos, read_pos);
      EXPECT_EQ(WritePosOf(rw_positions), write_pos);
      EXPECT_EQ(ReadPosOf(rw_positions), read_pos);
    }
  }
}

TEST(SharedRingBufferABITest, ReplaceReadPos) {
  // Only the read half changes.
  const uint64_t rw_positions = PackRwPositions(0x11111111u, 0x22222222u);
  const uint64_t moved = ReplaceReadPos(rw_positions, 0x22222223u);
  EXPECT_EQ(WritePosOf(moved), 0x11111111u);
  EXPECT_EQ(ReadPosOf(moved), 0x22222223u);
  EXPECT_EQ(ReplaceReadPos(PackRwPositions(7, 0xffffffffu), 0),
            PackRwPositions(7u, 0u));
}

// Logical positions
// -----------------

TEST(SharedRingBufferABITest, ChunkIndexAndWrapCount) {
  // A worked example.
  const uint32_t kNumChunks = 4;
  const uint32_t kExpectedIndex[] = {0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0};
  const uint32_t kExpectedWrap[] = {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3};
  for (uint32_t p = 0; p < 13; ++p) {
    EXPECT_EQ(ChunkIndexOfPosition(p, kNumChunks), kExpectedIndex[p]) << p;
    EXPECT_EQ(WrapCountForPosition(p, kNumChunks), kExpectedWrap[p]) << p;
  }

  // Including the one-chunk ring, where every position maps to chunk 0 and the
  // wrap count is the position itself, truncated.
  for (uint32_t num_chunks : {1u, 2u, 4u, 8u, 1024u}) {
    for (uint32_t p = 0; p < 3 * num_chunks + 3; ++p) {
      EXPECT_EQ(ChunkIndexOfPosition(p, num_chunks), p % num_chunks);
      EXPECT_EQ(WrapCountForPosition(p, num_chunks),
                (p / num_chunks) & 0xffffu);
    }
  }
}

TEST(SharedRingBufferABITest, OutstandingPositionsAcrossRollover) {
  // A worked example.
  EXPECT_EQ(NumOutstandingPositions(0x00000002u, 0xfffffffcu), 6u);
  EXPECT_EQ(NumOutstandingPositions(0u, 0u), 0u);
  EXPECT_EQ(NumOutstandingPositions(0u, 0xffffffffu), 1u);
  EXPECT_EQ(NumOutstandingPositions(0xffffffffu, 0xfffffffeu), 1u);

  const uint32_t kNumChunks = 8;
  const uint32_t kExpected[] = {4, 5, 6, 7, 0, 1};
  uint32_t p = 0xfffffffcu;
  for (uint32_t i = 0; i < 6; ++i, ++p)
    EXPECT_EQ(ChunkIndexOfPosition(p, kNumChunks), kExpected[i]) << i;
}

TEST(SharedRingBufferABITest, NextWrapFromPosition) {
  // The next wrap comes from the next position, not from the chunk word.
  // Away from the rollovers next_wrap is simply "one more".
  const uint32_t kNumChunks = 4;
  for (uint32_t p = 0; p < 16; ++p) {
    EXPECT_EQ(WrapCountForPosition(p + kNumChunks, kNumChunks),
              WrapCountForPosition(p, kNumChunks) + 1)
        << p;
  }

  // At the 16-bit truncation boundary it is not: the wrap after 0xffff is
  // zero. A reader that incremented the value it found in the chunk would
  // agree here by accident of the uint16_t, so the position rollover below is
  // the discriminating case.
  const uint32_t kLastLapOfPeriod = 0xffffu * kNumChunks;  // wrap 0xffff
  EXPECT_EQ(WrapCountForPosition(kLastLapOfPeriod, kNumChunks), 0xffffu);
  EXPECT_EQ(WrapCountForPosition(kLastLapOfPeriod + kNumChunks, kNumChunks),
            0u);

  // At the uint32_t position rollover the traversal number restarts from zero
  // mid-way through the 16-bit range whenever num_chunks > 65536, which is why
  // the protocol derives the wrap from the position and never increments the
  // value it finds in the chunk.
  const uint32_t kBigRing = 1u << 20;
  const uint32_t kLastPosition = 0u - kBigRing;  // the last lap's chunk 0
  EXPECT_EQ(ChunkIndexOfPosition(kLastPosition, kBigRing), 0u);
  EXPECT_EQ(WrapCountForPosition(kLastPosition, kBigRing), 0xfffu);
  EXPECT_EQ(WrapCountForPosition(kLastPosition + kBigRing, kBigRing), 0u);
  EXPECT_NE(WrapCountForPosition(kLastPosition + kBigRing, kBigRing),
            WrapCountForPosition(kLastPosition, kBigRing) + 1u);
}

// Pin the finite period of the 16-bit Free identity.
TEST(SharedRingBufferABITest, WrapIdentityPeriod) {
  // For num_chunks up to 65536 the identity period is num_chunks * 65536
  // reservations: the same chunk carries the same wrap count again once the
  // traversal number has run through the whole uint16_t.
  for (uint32_t num_chunks : {1u, 2u, 16u}) {
    const uint32_t period = num_chunks * 65536;
    EXPECT_EQ(WrapCountForPosition(0, num_chunks),
              WrapCountForPosition(period, num_chunks));
    EXPECT_EQ(ChunkIndexOfPosition(0, num_chunks),
              ChunkIndexOfPosition(period, num_chunks));
    // No earlier lap of the same chunk aliases position 0.
    for (uint32_t lap = 1; lap < 8; ++lap) {
      const uint32_t p = lap * num_chunks;
      EXPECT_NE(WrapCountForPosition(p, num_chunks),
                WrapCountForPosition(0, num_chunks));
    }
  }

  // From 65537 chunks up the truncation throws nothing away - the shifted
  // position already fits in 16 bits - so the identity repeats only when the
  // 32-bit position itself wraps: the period is min(num_chunks * 65536, 2^32).
  const uint32_t kBigRing = 1u << 20;
  EXPECT_EQ(WrapCountForPosition(UINT32_MAX, kBigRing), 0xfffu);
  for (uint32_t lap = 1; lap < 8; ++lap) {
    EXPECT_NE(WrapCountForPosition(lap * kBigRing, kBigRing),
              WrapCountForPosition(0, kBigRing));
  }
}

// Target-buffer chunk format
// --------------------------

TEST(SharedRingBufferABITest, FragmentSizeVarIntBytes) {
  EXPECT_EQ(FragmentSizeVarIntBytes(0), 1u);
  EXPECT_EQ(FragmentSizeVarIntBytes(127), 1u);
  EXPECT_EQ(FragmentSizeVarIntBytes(128), 2u);
  EXPECT_EQ(FragmentSizeVarIntBytes(16383), 2u);
  EXPECT_EQ(FragmentSizeVarIntBytes(16384), 3u);
  EXPECT_EQ(FragmentSizeVarIntBytes(0x1fffff), 3u);
  EXPECT_EQ(FragmentSizeVarIntBytes(0x200000), 4u);
  EXPECT_EQ(FragmentSizeVarIntBytes(0x0fffffff), 4u);
  EXPECT_EQ(FragmentSizeVarIntBytes(0x10000000), 5u);
  EXPECT_EQ(FragmentSizeVarIntBytes(UINT32_MAX), 5u);
}

TEST(SharedRingBufferABITest, MaxFragmentSizeForAvailableBytes) {
  // The largest fragment leaves room for its own varint.
  EXPECT_EQ(MaxFragmentSizeForAvailableBytes(0), 0u);
  EXPECT_EQ(MaxFragmentSizeForAvailableBytes(1), 0u);
  EXPECT_EQ(MaxFragmentSizeForAvailableBytes(2), 1u);
  EXPECT_EQ(MaxFragmentSizeForAvailableBytes(128), 127u);
  EXPECT_EQ(MaxFragmentSizeForAvailableBytes(129), 127u);
  EXPECT_EQ(MaxFragmentSizeForAvailableBytes(130), 128u);
  EXPECT_EQ(MaxFragmentSizeForAvailableBytes(250), 248u);
  EXPECT_EQ(MaxFragmentSizeForAvailableBytes(UINT32_MAX), UINT32_MAX - 5u);
}

TEST(SharedRingBufferABITest, MaxFragmentSizeForEmptyChunk) {
  EXPECT_EQ(MaxFragmentSizeForEmptyChunk(kMinChunkSize - 1), 0u);
  EXPECT_EQ(MaxFragmentSizeForEmptyChunk(256), 248u);
  EXPECT_EQ(MaxFragmentSizeForEmptyChunk(260), 252u);  // Non-power-of-two.
  EXPECT_EQ(MaxFragmentSizeForEmptyChunk(65536), 65527u);
  EXPECT_EQ(MaxFragmentSizeForEmptyChunk(128u * 1024), 128u * 1024 - 9u);
}

TEST(SharedRingBufferABITest, SizesGrowDown) {
  std::vector<uint8_t> chunk(256, 0);
  uint8_t* sizes_begin = chunk.data() + chunk.size();
  for (uint32_t size : {5u, 200u, 3u})
    sizes_begin = WriteFragmentSize(sizes_begin, size);

  // Fragment 0 is nearest the end. Reading towards lower addresses yields the
  // normal varint byte sequence c8 01 for 200.
  EXPECT_EQ(sizes_begin, chunk.data() + 252);
  EXPECT_EQ(chunk[252], 0x03u);
  EXPECT_EQ(chunk[253], 0x01u);
  EXPECT_EQ(chunk[254], 0xc8u);
  EXPECT_EQ(chunk[255], 0x05u);

  const uint8_t* cursor = chunk.data() + chunk.size();
  for (uint32_t expected : {5u, 200u, 3u}) {
    uint32_t actual = 0;
    ASSERT_TRUE(ReadFragmentSize(sizes_begin, &cursor, &actual));
    EXPECT_EQ(actual, expected);
  }
  EXPECT_EQ(cursor, sizes_begin);
}

TEST(SharedRingBufferABITest, FragmentSizeRoundTrip) {
  const uint32_t kSizes[] = {0,          1,          127,       128,
                             16383,      16384,      0x1fffff,  0x200000,
                             0x0fffffff, 0x10000000, UINT32_MAX};
  std::vector<uint8_t> sizes(64, 0xee);
  uint8_t* sizes_begin = sizes.data() + sizes.size();
  for (uint32_t size : kSizes)
    sizes_begin = WriteFragmentSize(sizes_begin, size);

  const uint8_t* cursor = sizes.data() + sizes.size();
  for (uint32_t expected : kSizes) {
    uint32_t actual = 0;
    ASSERT_TRUE(ReadFragmentSize(sizes_begin, &cursor, &actual));
    EXPECT_EQ(actual, expected);
  }
  EXPECT_EQ(cursor, sizes_begin);
}

TEST(SharedRingBufferABITest, MalformedFragmentSizes) {
  const uint8_t kUnterminated[] = {0x80};
  const uint8_t kTooLong[] = {0x00, 0x80, 0x80, 0x80, 0x80, 0x80};
  const uint8_t kUint32Overflow[] = {0x10, 0xff, 0xff, 0xff, 0xff};
  const uint8_t kNonCanonical[] = {0x00, 0x80};

  auto expect_rejected = [](const uint8_t* begin, size_t size) {
    const uint8_t* cursor = begin + size;
    uint32_t fragment_size = 0xdeadbeef;
    EXPECT_FALSE(ReadFragmentSize(begin, &cursor, &fragment_size));
    // A rejection leaves both outputs untouched.
    EXPECT_EQ(cursor, begin + size);
    EXPECT_EQ(fragment_size, 0xdeadbeefu);
  };
  expect_rejected(kUnterminated, sizeof(kUnterminated));
  expect_rejected(kTooLong, sizeof(kTooLong));
  expect_rejected(kUint32Overflow, sizeof(kUint32Overflow));
  expect_rejected(kNonCanonical, sizeof(kNonCanonical));
}

TEST(SharedRingBufferABITest, TargetBufferID) {
  // Stored little-endian at bytes 4 and 5.
  std::vector<uint8_t> chunk(256, 0);
  StoreTargetBufferID(chunk.data(), 0x1234);
  EXPECT_EQ(chunk[4], 0x34u);
  EXPECT_EQ(chunk[5], 0x12u);
  EXPECT_EQ(LoadTargetBufferID(chunk.data()), 0x1234);
  // The state word is not disturbed and the payload area still starts at 6.
  EXPECT_EQ(chunk[0], 0u);
  EXPECT_EQ(chunk[3], 0u);
  EXPECT_EQ(chunk[kTargetBufferPayloadOffset], 0u);

  StoreTargetBufferID(chunk.data(), 0xffff);
  EXPECT_EQ(LoadTargetBufferID(chunk.data()), 0xffff);
  StoreTargetBufferID(chunk.data(), 0);
  EXPECT_EQ(LoadTargetBufferID(chunk.data()), 0);
}

}  // namespace
}  // namespace perfetto::tracing_v2
