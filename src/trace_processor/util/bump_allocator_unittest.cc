/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/util/bump_allocator.h"

#include <limits>
#include <random>
#include <vector>

#include "perfetto/ext/base/utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {

class BumpAllocatorUnittest : public ::testing::Test {
 public:
  // Allocates |size| bytes of memory with aligned to |align|, writes |size|
  // bytes in the region, reads |size| bytes and then frees the memory.
  //
  // Very useful to check that none of the internal DCHECKs of the allocator
  // fire.
  void AllocateWriteReadAndFree(uint32_t size) {
    BumpAllocator::AllocId id = allocator_.Alloc(size);
    uint8_t* ptr = static_cast<uint8_t*>(allocator_.GetPointer(id));

    std::vector<uint8_t> data(size);
    for (uint32_t i = 0; i < size; ++i) {
      data[i] = static_cast<uint8_t>(rnd_engine_() &
                                     std::numeric_limits<uint8_t>::max());
    }
    memcpy(ptr, data.data(), size);
    ASSERT_EQ(memcmp(ptr, data.data(), size), 0);
    allocator_.Free(id);
  }

 protected:
  std::minstd_rand0 rnd_engine_;
  BumpAllocator allocator_;
};

TEST_F(BumpAllocatorUnittest, AllocSmoke) {
  AllocateWriteReadAndFree(8);
  AllocateWriteReadAndFree(16);
  AllocateWriteReadAndFree(24);
  AllocateWriteReadAndFree(64);
  AllocateWriteReadAndFree(1024);
  AllocateWriteReadAndFree(BumpAllocator::kChunkSize);

  allocator_.EraseFrontFreeChunks();
}

TEST_F(BumpAllocatorUnittest, EraseFrontAtAnyTime) {
  BumpAllocator::AllocId id = allocator_.Alloc(8);
  allocator_.EraseFrontFreeChunks();
  allocator_.Free(id);
  allocator_.EraseFrontFreeChunks();
}

TEST_F(BumpAllocatorUnittest, PastEndOnChunkBoundary) {
  BumpAllocator::AllocId id = allocator_.Alloc(BumpAllocator::kChunkSize);
  BumpAllocator::AllocId past_end = allocator_.PastTheEndId();
  ASSERT_GT(past_end, id);
  ASSERT_EQ(past_end.chunk_index, 1u);
  ASSERT_EQ(past_end.chunk_offset, 0u);
  allocator_.Free(id);
}

TEST_F(BumpAllocatorUnittest, EraseFrontAccounting) {
  AllocateWriteReadAndFree(8);
  ASSERT_EQ(allocator_.EraseFrontFreeChunks(), 1u);
  ASSERT_EQ(allocator_.erased_front_chunks_count(), 1u);
  AllocateWriteReadAndFree(8);
  ASSERT_EQ(allocator_.EraseFrontFreeChunks(), 1u);
  ASSERT_EQ(allocator_.erased_front_chunks_count(), 2u);
}

TEST_F(BumpAllocatorUnittest, EraseFrontFreeChunk) {
  AllocateWriteReadAndFree(8);
  allocator_.EraseFrontFreeChunks();

  auto past_id = allocator_.PastTheEndId();
  ASSERT_EQ(past_id.chunk_index, 1u);
  ASSERT_EQ(past_id.chunk_offset, 0u);

  auto id = allocator_.Alloc(8);
  ASSERT_EQ(id.chunk_index, past_id.chunk_index);
  ASSERT_EQ(id.chunk_offset, past_id.chunk_offset);
  allocator_.Free(id);
}

TEST_F(BumpAllocatorUnittest, StressTest) {
  std::minstd_rand0 rnd_engine;
  for (int i = 0; i < 1000; i++) {
    uint32_t size =
        static_cast<uint32_t>((rnd_engine() * 8) % BumpAllocator::kChunkSize);
    AllocateWriteReadAndFree(size);
  }
}

}  // namespace trace_processor
}  // namespace perfetto
