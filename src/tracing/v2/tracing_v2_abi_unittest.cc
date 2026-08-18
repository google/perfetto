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

#include "src/tracing/v2/tracing_v2_abi.h"

#include <stdint.h>

#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto::tracing_v2 {
namespace {

// These words are the ABI written down in hex, so they are checked
// here as literals rather than recomputed from the same formulas.
TEST(TracingV2AbiTest, ConcreteStateWordsFromTheRfc) {
  EXPECT_EQ(MakeFreeForWrapWord(0), 0x00000000u);
  EXPECT_EQ(MakeFreeForWrapWord(5), 0x00000005u);
  EXPECT_EQ(MakeDataBearingWord(ChunkState::kAcquired,
                                ChunkFormat::kTargetBuffer, 0, 0, 7),
            0x20000007u);
  EXPECT_EQ(MakeDataBearingWord(ChunkState::kComplete,
                                ChunkFormat::kTargetBuffer, 0, 3, 7),
            0x40030007u);
  EXPECT_EQ(MakeDataBearingWord(ChunkState::kRewriteRequested,
                                ChunkFormat::kTargetBuffer, 0, 3, 7),
            0x60030007u);
  EXPECT_EQ(
      MakeDataBearingWord(ChunkState::kComplete, ChunkFormat::kTargetBuffer,
                          kFlagContinuesOnNextChunk, 2, 0x1234),
      0x42021234u);
  EXPECT_EQ(kAcknowledgedWord, 0x80000000u);
}

TEST(TracingV2AbiTest, StateOrdinalsAndTheirDecoding) {
  EXPECT_EQ(StateOf(0x00000000u), ChunkState::kFreeForWrap);
  EXPECT_EQ(StateOf(0x20000000u), ChunkState::kAcquired);
  EXPECT_EQ(StateOf(0x40000000u), ChunkState::kComplete);
  EXPECT_EQ(StateOf(0x60000000u), ChunkState::kRewriteRequested);
  EXPECT_EQ(StateOf(0x80000000u), ChunkState::kAcknowledged);
  EXPECT_EQ(StateOf(0xa0000000u), ChunkState::kReserved5);
  EXPECT_EQ(StateOf(0xc0000000u), ChunkState::kReserved6);
  EXPECT_EQ(StateOf(0xe0000000u), ChunkState::kReserved7);

  EXPECT_TRUE(IsDataBearing(ChunkState::kAcquired));
  EXPECT_TRUE(IsDataBearing(ChunkState::kComplete));
  EXPECT_TRUE(IsDataBearing(ChunkState::kRewriteRequested));
  EXPECT_FALSE(IsDataBearing(ChunkState::kFreeForWrap));
  EXPECT_FALSE(IsDataBearing(ChunkState::kAcknowledged));
  EXPECT_FALSE(IsDataBearing(ChunkState::kReserved5));
  EXPECT_FALSE(IsDataBearing(ChunkState::kReserved6));
  EXPECT_FALSE(IsDataBearing(ChunkState::kReserved7));
}

TEST(TracingV2AbiTest, DataBearingFieldsRoundTrip) {
  const ChunkState kStates[] = {ChunkState::kAcquired, ChunkState::kComplete,
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
          const uint32_t word = MakeDataBearingWord(
              state, ChunkFormat::kTargetBuffer, flags, count, writer);
          EXPECT_EQ(StateOf(word), state);
          EXPECT_EQ(FormatOf(word), ChunkFormat::kTargetBuffer);
          EXPECT_EQ(PayloadFlagsOf(word), flags);
          EXPECT_EQ(NumFragmentsOf(word), count);
          EXPECT_EQ(WriterIdOf(word), writer);
        }
      }
    }
  }
}

TEST(TracingV2AbiTest, EveryFormatRoundTrips) {
  const ChunkFormat kFormats[] = {
      ChunkFormat::kTargetBuffer, ChunkFormat::kReservedRouting,
      ChunkFormat::kReserved2, ChunkFormat::kReserved3};
  for (ChunkFormat format : kFormats) {
    const uint32_t word = MakeDataBearingWord(ChunkState::kComplete, format,
                                              kFlagDataLoss, 9, 0xabcd);
    EXPECT_EQ(FormatOf(word), format);
    // Whatever the format is, the state and the fields the reader needs to
    // arbitrate ownership stay where they are. That is what lets an old reader
    // release a chunk whose layout it has never heard of.
    EXPECT_EQ(StateOf(word), ChunkState::kComplete);
    EXPECT_EQ(NumFragmentsOf(word), 9u);
    EXPECT_EQ(WriterIdOf(word), 0xabcd);
  }
}

TEST(TracingV2AbiTest, WithStateReplacesOnlyTheState) {
  const uint32_t acquired = MakeDataBearingWord(
      ChunkState::kAcquired, ChunkFormat::kReservedRouting,
      kFlagContinuesFromPrevChunk | kFlagDataLoss, 17, 0x0f0f);
  const uint32_t marked = WithState(acquired, ChunkState::kRewriteRequested);

  EXPECT_EQ(StateOf(marked), ChunkState::kRewriteRequested);
  EXPECT_EQ(acquired & ~kStateMask, marked & ~kStateMask);
  EXPECT_EQ(FormatOf(marked), ChunkFormat::kReservedRouting);
  EXPECT_EQ(PayloadFlagsOf(marked),
            kFlagContinuesFromPrevChunk | kFlagDataLoss);
  EXPECT_EQ(NumFragmentsOf(marked), 17u);
  EXPECT_EQ(WriterIdOf(marked), 0x0f0f);
}

TEST(TracingV2AbiTest, FreeForWrapCarriesNothingButTheWrapCount) {
  EXPECT_EQ(WrapCountOf(MakeFreeForWrapWord(0)), 0u);
  EXPECT_EQ(WrapCountOf(MakeFreeForWrapWord(1)), 1u);
  EXPECT_EQ(WrapCountOf(MakeFreeForWrapWord(kWrapCountMask)), kWrapCountMask);

  // The wrap count is 29 bits: anything above is masked off rather than
  // spilling into the state field and turning a free chunk into an owned one.
  EXPECT_EQ(StateOf(MakeFreeForWrapWord(0xffffffffu)),
            ChunkState::kFreeForWrap);
  EXPECT_EQ(WrapCountOf(MakeFreeForWrapWord(0xffffffffu)), kWrapCountMask);
}

// ---------------------------------------------------------------------------
// Positions, chunk indices and wrap counts.
// ---------------------------------------------------------------------------

TEST(TracingV2AbiTest, ChunkIndexAndWrapCountForSeveralRingSizes) {
  // A worked example.
  const uint32_t kNumChunks = 4;
  const uint32_t kBits = Log2ForPowerOfTwo(kNumChunks);
  const uint32_t kExpectedIndex[] = {0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0};
  const uint32_t kExpectedWrap[] = {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3};
  for (uint32_t p = 0; p < 13; ++p) {
    EXPECT_EQ(ChunkIndexOfPosition(p, kNumChunks), kExpectedIndex[p]) << p;
    EXPECT_EQ(WrapCountOfPosition(p, kBits), kExpectedWrap[p]) << p;
  }

  for (uint32_t num_chunks : {2u, 4u, 8u, 1024u}) {
    const uint32_t bits = Log2ForPowerOfTwo(num_chunks);
    for (uint32_t p = 0; p < 3 * num_chunks; ++p) {
      EXPECT_EQ(ChunkIndexOfPosition(p, num_chunks), p % num_chunks);
      EXPECT_EQ(WrapCountOfPosition(p, bits), p / num_chunks);
    }
  }
}

TEST(TracingV2AbiTest, DistanceIsExactAcrossTheCursorRollover) {
  // A worked example.
  EXPECT_EQ(PositionDistance(0x00000002u, 0xfffffffcu), 6u);
  EXPECT_EQ(PositionDistance(0u, 0u), 0u);
  EXPECT_EQ(PositionDistance(0u, 0xffffffffu), 1u);
  EXPECT_EQ(PositionDistance(0xffffffffu, 0xfffffffeu), 1u);

  const uint32_t kNumChunks = 8;
  const uint32_t kExpected[] = {4, 5, 6, 7, 0, 1};
  uint32_t p = 0xfffffffcu;
  for (uint32_t i = 0; i < 6; ++i, ++p)
    EXPECT_EQ(ChunkIndexOfPosition(p, kNumChunks), kExpected[i]) << i;
}

TEST(TracingV2AbiTest, NextWrapComesFromTheNextPositionNotFromTheChunk) {
  // Away from the rollover next_wrap is simply "one more".
  const uint32_t kNumChunks = 4;
  const uint32_t kBits = Log2ForPowerOfTwo(kNumChunks);
  for (uint32_t p = 0; p < 16; ++p) {
    EXPECT_EQ(NextWrapCount(p, kNumChunks, kBits),
              WrapCountOfPosition(p, kBits) + 1)
        << p;
  }

  // At the uint32_t rollover it is not. With more than eight chunks the wrap
  // count returns to zero long before all 29 bits have been used, which is
  // exactly why the protocol derives it from the position and never increments
  // the value it finds in the chunk.
  const uint32_t kBigRing = 16;
  const uint32_t kBigBits = Log2ForPowerOfTwo(kBigRing);
  const uint32_t kLastPosition = 0xfffffff0u;  // maps to chunk 0
  EXPECT_EQ(ChunkIndexOfPosition(kLastPosition, kBigRing), 0u);
  EXPECT_EQ(WrapCountOfPosition(kLastPosition, kBigBits), 0x0fffffffu);
  EXPECT_EQ(NextWrapCount(kLastPosition, kBigRing, kBigBits), 0u);
  EXPECT_NE(NextWrapCount(kLastPosition, kBigRing, kBigBits),
            WrapCountOfPosition(kLastPosition, kBigBits) + 1);
}

// The 29-bit tag is finite, so it repeats. The protocol accepts that rather
// than designing it away; this test pins how long it lasts so the bound is a
// checked number and not a claim in a comment.
TEST(TracingV2AbiTest, WrapIdentityRepeatsAfterTheDocumentedPeriod) {
  // With two chunks the mask throws bits away: the identity period is
  // num_chunks * 2^29 = 2^30 reservations, not 2^32.
  const uint32_t kNumChunks = 2;
  const uint32_t kBits = Log2ForPowerOfTwo(kNumChunks);
  const uint32_t kPeriod = kNumChunks << 29;  // 2^30
  EXPECT_EQ(WrapCountOfPosition(0, kBits), WrapCountOfPosition(kPeriod, kBits));
  EXPECT_EQ(ChunkIndexOfPosition(0, kNumChunks),
            ChunkIndexOfPosition(kPeriod, kNumChunks));
  // Nothing before the period aliases position 0 on the same chunk.
  for (uint32_t lap = 1; lap < 8; ++lap) {
    const uint32_t p = lap * kNumChunks;
    EXPECT_NE(WrapCountOfPosition(p, kBits), WrapCountOfPosition(0, kBits));
  }

  // From eight chunks up the mask throws nothing away: the shifted position
  // still fits in 29 bits, so the stored identity is as precise as a 32-bit
  // logical position can be and repeats only when the position itself wraps.
  const uint32_t kWideRing = 8;
  const uint32_t kWideBits = Log2ForPowerOfTwo(kWideRing);
  EXPECT_EQ(WrapCountOfPosition(UINT32_MAX, kWideBits), 0x1fffffffu);
  const uint32_t kLastLap = 0xfffffff8u;  // the last lap's chunk 0
  EXPECT_EQ(ChunkIndexOfPosition(kLastLap, kWideRing), 0u);
  EXPECT_EQ(WrapCountOfPosition(kLastLap, kWideBits), 0x1fffffffu);
  EXPECT_EQ(NextWrapCount(kLastLap, kWideRing, kWideBits), 0u);
}

// ---------------------------------------------------------------------------
// The bidirectional payload area.
// ---------------------------------------------------------------------------

TEST(TracingV2AbiTest, FragmentSizeWidthFollowsTheChunkSizeTable) {
  EXPECT_EQ(FragmentSizeWidth(256), 1u);
  EXPECT_EQ(FragmentSizeWidth(512), 2u);
  EXPECT_EQ(FragmentSizeWidth(1024), 2u);
  EXPECT_EQ(FragmentSizeWidth(4096), 2u);
  EXPECT_EQ(FragmentSizeWidth(32768), 2u);
}

TEST(TracingV2AbiTest, DirectoryEntriesGrowDownFromTheEndOfTheChunk) {
  // A 256-byte target-buffer chunk holding
  // three fragments of 5, 200 and 3 bytes, with one-byte entries.
  std::vector<uint8_t> chunk(256, 0);
  const uint32_t w = FragmentSizeWidth(256);
  const uint32_t kSizes[] = {5, 200, 3};
  for (uint32_t i = 0; i < 3; ++i) {
    StoreFragmentSize(chunk.data() + FragmentSizeEntryOffset(256, w, i), w,
                      kSizes[i]);
  }

  EXPECT_EQ(chunk[255], 0x05u);
  EXPECT_EQ(chunk[254], 0xc8u);
  EXPECT_EQ(chunk[253], 0x03u);
  // Three fragments cost exactly three directory bytes.
  EXPECT_EQ(chunk[252], 0x00u);

  for (uint32_t i = 0; i < 3; ++i) {
    EXPECT_EQ(
        LoadFragmentSize(chunk.data() + FragmentSizeEntryOffset(256, w, i), w),
        kSizes[i]);
  }
}

TEST(TracingV2AbiTest, TwoByteDirectoryEntriesAreLittleEndian) {
  std::vector<uint8_t> chunk(512, 0);
  const uint32_t w = FragmentSizeWidth(512);
  ASSERT_EQ(w, 2u);

  StoreFragmentSize(chunk.data() + FragmentSizeEntryOffset(512, w, 0), w,
                    0x0102);
  EXPECT_EQ(chunk[510], 0x02u);
  EXPECT_EQ(chunk[511], 0x01u);

  StoreFragmentSize(chunk.data() + FragmentSizeEntryOffset(512, w, 1), w, 255);
  EXPECT_EQ(chunk[508], 0xffu);
  EXPECT_EQ(chunk[509], 0x00u);

  EXPECT_EQ(
      LoadFragmentSize(chunk.data() + FragmentSizeEntryOffset(512, w, 0), w),
      0x0102u);
  EXPECT_EQ(
      LoadFragmentSize(chunk.data() + FragmentSizeEntryOffset(512, w, 1), w),
      255u);
}

TEST(TracingV2AbiTest, DirectoryEntriesRoundTripAtInterestingSizes) {
  for (uint32_t chunk_size : {256u, 512u, 32768u}) {
    const uint32_t w = FragmentSizeWidth(chunk_size);
    const uint32_t max_size = w == 1 ? 255u : 65535u;
    std::vector<uint8_t> chunk(chunk_size, 0xee);
    const uint32_t kSizes[] = {0, 1, 127, 128, 254, 255};
    for (uint32_t i = 0; i < 6; ++i) {
      StoreFragmentSize(
          chunk.data() + FragmentSizeEntryOffset(chunk_size, w, i), w,
          kSizes[i]);
    }
    for (uint32_t i = 0; i < 6; ++i) {
      EXPECT_EQ(
          LoadFragmentSize(
              chunk.data() + FragmentSizeEntryOffset(chunk_size, w, i), w),
          kSizes[i])
          << chunk_size << "/" << i;
    }
    StoreFragmentSize(chunk.data() + FragmentSizeEntryOffset(chunk_size, w, 6),
                      w, max_size);
    EXPECT_EQ(LoadFragmentSize(
                  chunk.data() + FragmentSizeEntryOffset(chunk_size, w, 6), w),
              max_size);
  }
}

TEST(TracingV2AbiTest, TargetBufferIsLittleEndianAtBytesFourAndFive) {
  std::vector<uint8_t> chunk(256, 0);
  StoreTargetBuffer(chunk.data(), 0x1234);
  EXPECT_EQ(chunk[4], 0x34u);
  EXPECT_EQ(chunk[5], 0x12u);
  EXPECT_EQ(LoadTargetBuffer(chunk.data()), 0x1234);
  // The state word is not disturbed and the payload area still starts at 6.
  EXPECT_EQ(chunk[0], 0u);
  EXPECT_EQ(chunk[3], 0u);
  EXPECT_EQ(chunk[kTargetBufferPayloadOffset], 0u);

  StoreTargetBuffer(chunk.data(), 0xffff);
  EXPECT_EQ(LoadTargetBuffer(chunk.data()), 0xffff);
  StoreTargetBuffer(chunk.data(), 0);
  EXPECT_EQ(LoadTargetBuffer(chunk.data()), 0);
}

}  // namespace
}  // namespace perfetto::tracing_v2
