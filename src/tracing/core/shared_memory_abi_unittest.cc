/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "perfetto/tracing/core/shared_memory_abi.h"

#include "gtest/gtest.h"
#include "perfetto/tracing/core/basic_types.h"
#include "src/tracing/test/aligned_buffer_test.h"

namespace perfetto {
namespace {

using testing::ValuesIn;
using Chunk = SharedMemoryABI::Chunk;
using ChunkHeader = SharedMemoryABI::ChunkHeader;

using SharedMemoryABITest = AlignedBufferTest;

size_t const kPageSizes[] = {4096, 8192, 16384, 32768, 65536};
INSTANTIATE_TEST_CASE_P(PageSize, SharedMemoryABITest, ValuesIn(kPageSizes));

TEST_P(SharedMemoryABITest, NominalCases) {
  SharedMemoryABI abi(buf(), buf_size(), page_size());

  ASSERT_EQ(buf(), abi.start());
  ASSERT_EQ(buf() + buf_size(), abi.end());
  ASSERT_EQ(buf_size(), abi.size());
  ASSERT_EQ(page_size(), abi.page_size());
  ASSERT_EQ(kNumPages, abi.num_pages());

  for (size_t i = 0; i < kNumPages; i++) {
    ASSERT_TRUE(abi.is_page_free(i));
    ASSERT_FALSE(abi.is_page_complete(i));
    // GetFreeChunks() should return 0 for an unpartitioned page.
    ASSERT_EQ(0u, abi.GetFreeChunks(i));
  }

  ASSERT_TRUE(abi.TryPartitionPage(0, SharedMemoryABI::kPageDiv1));
  ASSERT_EQ(0x01u, abi.GetFreeChunks(0));

  ASSERT_TRUE(abi.TryPartitionPage(1, SharedMemoryABI::kPageDiv2));
  ASSERT_EQ(0x03u, abi.GetFreeChunks(1));

  ASSERT_TRUE(abi.TryPartitionPage(2, SharedMemoryABI::kPageDiv4));
  ASSERT_EQ(0x0fu, abi.GetFreeChunks(2));

  ASSERT_TRUE(abi.TryPartitionPage(3, SharedMemoryABI::kPageDiv7));
  ASSERT_EQ(0x7fu, abi.GetFreeChunks(3));

  ASSERT_TRUE(abi.TryPartitionPage(4, SharedMemoryABI::kPageDiv14));
  ASSERT_EQ(0x3fffu, abi.GetFreeChunks(4));

  // Repartitioning an existing page must fail.
  ASSERT_FALSE(abi.TryPartitionPage(0, SharedMemoryABI::kPageDiv1));
  ASSERT_FALSE(abi.TryPartitionPage(4, SharedMemoryABI::kPageDiv14));

  for (size_t i = 0; i <= 4; i++) {
    ASSERT_FALSE(abi.is_page_free(i));
    ASSERT_FALSE(abi.is_page_complete(i));
  }

  uint16_t last_chunk_id = 0;
  uint16_t last_writer_id = 0;
  uint8_t* last_chunk_begin = nullptr;
  uint8_t* last_chunk_end = nullptr;

  for (size_t page_idx = 0; page_idx <= 4; page_idx++) {
    uint8_t* const page_start = buf() + page_idx * page_size();
    uint8_t* const page_end = page_start + page_size();
    const size_t num_chunks =
        SharedMemoryABI::GetNumChunksForLayout(abi.page_layout_dbg(page_idx));
    Chunk chunks[14];

    for (size_t chunk_idx = 0; chunk_idx < num_chunks; chunk_idx++) {
      Chunk& chunk = chunks[chunk_idx];
      ChunkHeader header{};

      ASSERT_EQ(SharedMemoryABI::kChunkFree,
                abi.GetChunkState(page_idx, chunk_idx));
      uint16_t chunk_id = ++last_chunk_id;
      last_writer_id = (last_writer_id + 1) & kMaxWriterID;
      uint16_t writer_id = last_writer_id;
      header.chunk_id.store(chunk_id);
      header.writer_id.store(writer_id);

      uint16_t packets_count = static_cast<uint16_t>(chunk_idx * 10);
      const uint8_t kFlagsMask = (1 << 6) - 1;
      uint8_t flags = static_cast<uint8_t>((0xffu - chunk_idx) & kFlagsMask);
      header.packets.store({packets_count, flags});

      chunk = abi.TryAcquireChunkForWriting(page_idx, chunk_idx, &header);
      ASSERT_TRUE(chunk.is_valid());
      ASSERT_EQ(SharedMemoryABI::kChunkBeingWritten,
                abi.GetChunkState(page_idx, chunk_idx));

      // Sanity check chunk bounds.
      size_t expected_chunk_size =
          (page_size() - sizeof(SharedMemoryABI::PageHeader)) / num_chunks;
      expected_chunk_size = expected_chunk_size - (expected_chunk_size % 4);
      ASSERT_EQ(expected_chunk_size, chunk.size());
      ASSERT_EQ(expected_chunk_size - sizeof(SharedMemoryABI::ChunkHeader),
                chunk.payload_size());
      ASSERT_GT(chunk.begin(), page_start);
      ASSERT_GT(chunk.begin(), last_chunk_begin);
      ASSERT_GE(chunk.begin(), last_chunk_end);
      ASSERT_LE(chunk.end(), page_end);
      ASSERT_GT(chunk.end(), chunk.begin());
      ASSERT_EQ(chunk.end(), chunk.begin() + chunk.size());
      last_chunk_begin = chunk.begin();
      last_chunk_end = chunk.end();

      ASSERT_EQ(chunk_id, chunk.header()->chunk_id.load());
      ASSERT_EQ(writer_id, chunk.header()->writer_id.load());
      ASSERT_EQ(packets_count, chunk.header()->packets.load().count);
      ASSERT_EQ(flags, chunk.header()->packets.load().flags);
      ASSERT_EQ(std::make_pair(packets_count, flags),
                chunk.GetPacketCountAndFlags());

      chunk.IncrementPacketCount();
      ASSERT_EQ(packets_count + 1, chunk.header()->packets.load().count);

      chunk.IncrementPacketCount();
      ASSERT_EQ(packets_count + 2, chunk.header()->packets.load().count);

      chunk.SetFlag(
          SharedMemoryABI::ChunkHeader::kLastPacketContinuesOnNextChunk);
      ASSERT_TRUE(
          chunk.header()->packets.load().flags &
          SharedMemoryABI::ChunkHeader::kLastPacketContinuesOnNextChunk);

      // Reacquiring the same chunk should fail.
      ASSERT_FALSE(abi.TryAcquireChunkForWriting(page_idx, chunk_idx, &header)
                       .is_valid());
    }

    // Now release chunks and check the Release() logic.
    for (size_t chunk_idx = 0; chunk_idx < num_chunks; chunk_idx++) {
      Chunk& chunk = chunks[chunk_idx];

      size_t res = abi.ReleaseChunkAsComplete(std::move(chunk));
      ASSERT_EQ(page_idx, res);
      ASSERT_EQ(chunk_idx == num_chunks - 1, abi.is_page_complete(page_idx));
      ASSERT_EQ(SharedMemoryABI::kChunkComplete,
                abi.GetChunkState(page_idx, chunk_idx));
    }

    // Now acquire all chunks for reading.
    for (size_t chunk_idx = 0; chunk_idx < num_chunks; chunk_idx++) {
      Chunk& chunk = chunks[chunk_idx];
      chunk = abi.TryAcquireChunkForReading(page_idx, chunk_idx);
      ASSERT_TRUE(chunk.is_valid());
      ASSERT_EQ(SharedMemoryABI::kChunkBeingRead,
                abi.GetChunkState(page_idx, chunk_idx));
    }

    // Finally release all chunks as free.
    for (size_t chunk_idx = 0; chunk_idx < num_chunks; chunk_idx++) {
      Chunk& chunk = chunks[chunk_idx];

      // If this was the last chunk in the page, the full page should be marked
      // as free.
      size_t res = abi.ReleaseChunkAsFree(std::move(chunk));
      ASSERT_EQ(page_idx, res);
      ASSERT_EQ(chunk_idx == num_chunks - 1, abi.is_page_free(page_idx));
      ASSERT_EQ(SharedMemoryABI::kChunkFree,
                abi.GetChunkState(page_idx, chunk_idx));
    }
  }
}

TEST_P(SharedMemoryABITest, BatchAcquireAndRelease) {
  SharedMemoryABI abi(buf(), buf_size(), page_size());
  ChunkHeader header{};

  // TryAcquire on a non-partitioned page should fail.
  ASSERT_FALSE(abi.TryAcquireChunkForWriting(0, 0, &header).is_valid());
  ASSERT_FALSE(abi.TryAcquireChunkForReading(0, 0).is_valid());

  // Now partition the page in one whole chunk.
  ASSERT_TRUE(abi.TryPartitionPage(0, SharedMemoryABI::kPageDiv1));

  Chunk chunk = abi.TryAcquireChunkForWriting(0, 0, &header);
  ASSERT_TRUE(chunk.is_valid());

  // TryAcquireAllChunksForReading() should fail, as the chunk is being written.
  ASSERT_FALSE(abi.TryAcquireAllChunksForReading(0));

  ASSERT_EQ(0u, abi.ReleaseChunkAsComplete(std::move(chunk)));
  ASSERT_FALSE(chunk.is_valid());

  // TryAcquireAllChunksForReading() should succeed given that the page has only
  // one chunk and is now complete.
  ASSERT_TRUE(abi.TryAcquireAllChunksForReading(0));

  // Release the one chunk and check that the page is freed up.
  abi.ReleaseAllChunksAsFree(0);
  ASSERT_TRUE(abi.is_page_free(0));

  // Now repartition the page into four chunks and try some trickier cases.
  ASSERT_TRUE(abi.TryPartitionPage(0, SharedMemoryABI::kPageDiv4));

  // Acquire only the first and last chunks.
  Chunk chunk0 = abi.TryAcquireChunkForWriting(0, 0, &header);
  ASSERT_TRUE(chunk0.is_valid());
  Chunk chunk3 = abi.TryAcquireChunkForWriting(0, 3, &header);
  ASSERT_TRUE(chunk3.is_valid());

  // TryAcquireAllChunksForReading() should fail, some chunks are being written.
  ASSERT_FALSE(abi.TryAcquireAllChunksForReading(0));

  // Mark only one chunks as complete and try again, it should still fail.
  ASSERT_EQ(0u, abi.ReleaseChunkAsComplete(std::move(chunk0)));
  ASSERT_FALSE(chunk0.is_valid());

  ASSERT_EQ(SharedMemoryABI::kChunkComplete, abi.GetChunkState(0, 0));
  ASSERT_EQ(SharedMemoryABI::kChunkFree, abi.GetChunkState(0, 1));
  ASSERT_EQ(SharedMemoryABI::kChunkFree, abi.GetChunkState(0, 2));
  ASSERT_EQ(SharedMemoryABI::kChunkBeingWritten, abi.GetChunkState(0, 3));
  ASSERT_FALSE(abi.TryAcquireAllChunksForReading(0));

  // Now release also the last chunk as complete and try again the
  // TryAcquireAllChunksForReading(). This time it should succeed.
  ASSERT_EQ(0u, abi.ReleaseChunkAsComplete(std::move(chunk3)));
  ASSERT_FALSE(chunk3.is_valid());

  ASSERT_EQ(SharedMemoryABI::kChunkComplete, abi.GetChunkState(0, 0));
  ASSERT_EQ(SharedMemoryABI::kChunkFree, abi.GetChunkState(0, 1));
  ASSERT_EQ(SharedMemoryABI::kChunkFree, abi.GetChunkState(0, 2));
  ASSERT_EQ(SharedMemoryABI::kChunkComplete, abi.GetChunkState(0, 3));
  ASSERT_TRUE(abi.TryAcquireAllChunksForReading(0));

  // At this point the two outer chunks should transition into the
  // kChunkBeingRead state, while the middle ones should stay free.
  ASSERT_EQ(SharedMemoryABI::kChunkBeingRead, abi.GetChunkState(0, 0));
  ASSERT_EQ(SharedMemoryABI::kChunkFree, abi.GetChunkState(0, 1));
  ASSERT_EQ(SharedMemoryABI::kChunkFree, abi.GetChunkState(0, 2));
  ASSERT_EQ(SharedMemoryABI::kChunkBeingRead, abi.GetChunkState(0, 3));

  // Release only one chunk as free.
  abi.ReleaseChunkAsFree(abi.GetChunkUnchecked(0, abi.page_layout_dbg(0), 0));
  ASSERT_EQ(SharedMemoryABI::kChunkFree, abi.GetChunkState(0, 0));
  ASSERT_EQ(SharedMemoryABI::kChunkBeingRead, abi.GetChunkState(0, 3));

  // Release the last chunk as free, the full page should be freed.
  abi.ReleaseChunkAsFree(abi.GetChunkUnchecked(0, abi.page_layout_dbg(0), 3));
  ASSERT_TRUE(abi.is_page_free(0));
}

}  // namespace
}  // namespace perfetto
