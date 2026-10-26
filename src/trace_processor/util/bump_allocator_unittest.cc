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

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <random>
#include <vector>

#include "perfetto/trace_processor/ref_counted.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "src/trace_processor/util/chunk_memory.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {

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

TEST_F(BumpAllocatorUnittest, Clear) {
  for (uint32_t i = 0; i < 3; ++i) {
    allocator_.Alloc(BumpAllocator::kChunkSize);
  }
  BumpAllocator::AllocId before = allocator_.PastTheEndId();
  allocator_.Clear();
  BumpAllocator::AllocId after = allocator_.Alloc(8);
  EXPECT_TRUE(after >= before);
  EXPECT_EQ(after.chunk_offset, 0u);
  allocator_.Clear();
}

TEST_F(BumpAllocatorUnittest, StressTest) {
  std::minstd_rand0 rnd_engine;
  for (int i = 0; i < 1000; i++) {
    uint32_t size =
        static_cast<uint32_t>((rnd_engine() * 8) % BumpAllocator::kChunkSize);
    AllocateWriteReadAndFree(size);
  }
}

// Writes chunks far past the resident budget and reads them back out of
// order. A pinned chunk and a chunk with a live view are never paged out.
TEST_F(BumpAllocatorUnittest, PagingRoundTrip) {
  RefPtr<ChunkMemory> paged =
      ChunkMemory::Paged(BumpAllocator::kChunkSize, BumpAllocator::kChunkSize);
  if (!paged) {
    GTEST_SKIP() << "Paging is not supported here";
  }
  BumpAllocator allocator(paged);
  void* pinned_ptr;
  BumpAllocator::AllocId pinned = allocator.Alloc(8, &pinned_ptr);
  allocator.PinLastChunk();

  constexpr uint32_t kAllocs = 32;
  constexpr uint32_t kViewed = 10;
  constexpr uint32_t kSize = BumpAllocator::kChunkSize / 2;
  std::vector<BumpAllocator::AllocId> ids;
  void* viewed_ptr = nullptr;
  RefPtr<TraceBlob> view;
  for (uint32_t i = 0; i < kAllocs; ++i) {
    void* ptr;
    ids.push_back(allocator.Alloc(kSize, &ptr));
    memset(ptr, static_cast<int>(i), kSize);
    if (i == kViewed) {
      viewed_ptr = ptr;
      view = allocator.GetAllocation(ids.back()).chunk_blob();
    }
  }

  // A paged in chunk gets new memory, so an unchanged pointer means the
  // chunk was never paged out.
  ASSERT_EQ(allocator.GetPointer(pinned), pinned_ptr);
  ASSERT_EQ(allocator.GetPointer(ids[kViewed]), viewed_ptr);

  // Read back in a different order to the writes, so chunks are paged in
  // while others are still out.
  for (uint32_t n = 0; n < kAllocs; ++n) {
    uint32_t i = (n * 7) % kAllocs;
    auto* ptr = static_cast<uint8_t*>(allocator.GetPointer(ids[i]));
    ASSERT_EQ(ptr[0], static_cast<uint8_t>(i));
    ASSERT_EQ(ptr[kSize - 1], static_cast<uint8_t>(i));
  }
  allocator.Free(pinned);
  for (BumpAllocator::AllocId id : ids) {
    allocator.Free(id);
  }
  allocator.EraseFrontFreeChunks();
}

TEST_F(BumpAllocatorUnittest, ManyMoreChunksThanBudget) {
  constexpr uint32_t kBudgetChunks = 4;
  RefPtr<ChunkMemory> paged = ChunkMemory::Paged(
      BumpAllocator::kChunkSize, kBudgetChunks * BumpAllocator::kChunkSize);
  if (!paged) {
    GTEST_SKIP() << "Paging is not supported here";
  }
  BumpAllocator allocator(paged);
  constexpr uint32_t kSize = 1024;
  constexpr uint32_t kPerChunk = BumpAllocator::kChunkSize / kSize;
  std::vector<BumpAllocator::AllocId> ids;
  uint32_t next_to_free = 0;
  // Frees and erases the oldest chunks while appending, so paged out slots
  // are freed and reused.
  for (uint32_t round = 0; round < 8; ++round) {
    for (uint32_t i = 0; i < 16 * kPerChunk; ++i) {
      void* ptr;
      uint32_t n = static_cast<uint32_t>(ids.size());
      ids.push_back(allocator.Alloc(kSize, &ptr));
      memset(ptr, static_cast<int>(n % 251), kSize);
    }
    for (uint32_t i = 0; i < 8 * kPerChunk; ++i, ++next_to_free) {
      auto* ptr =
          static_cast<uint8_t*>(allocator.GetPointer(ids[next_to_free]));
      ASSERT_EQ(ptr[0], static_cast<uint8_t>(next_to_free % 251));
      ASSERT_EQ(ptr[kSize - 1], static_cast<uint8_t>(next_to_free % 251));
      allocator.Free(ids[next_to_free]);
    }
    ASSERT_EQ(allocator.EraseFrontFreeChunks(), 8u);
  }
  for (; next_to_free < ids.size(); ++next_to_free) {
    auto* ptr = static_cast<uint8_t*>(allocator.GetPointer(ids[next_to_free]));
    ASSERT_EQ(ptr[0], static_cast<uint8_t>(next_to_free % 251));
    allocator.Free(ids[next_to_free]);
  }
  allocator.EraseFrontFreeChunks();
}

// Pages chunks out to memory, counting how often each slot is freed.
class FakePagedMemory : public ChunkMemory {
 public:
  explicit FakePagedMemory(size_t resident_budget)
      : ChunkMemory(BumpAllocator::kChunkSize, resident_budget) {}

  uint32_t PageOut(const uint8_t* data, size_t size) override {
    uint32_t slot = next_slot_++;
    pages_[slot].assign(data, data + size);
    return slot;
  }
  void PageIn(uint32_t slot, uint8_t* data, size_t size) override {
    auto it = pages_.find(slot);
    PERFETTO_CHECK(it != pages_.end() && it->second.size() == size);
    memcpy(data, it->second.data(), size);
    pages_.erase(it);
  }
  void FreePage(uint32_t slot) override {
    // Freeing a slot twice, or one paged back in, is a bug.
    PERFETTO_CHECK(pages_.erase(slot) == 1);
    frees_++;
  }

  size_t live_pages() const { return pages_.size(); }
  uint32_t frees() const { return frees_; }

 private:
  std::map<uint32_t, std::vector<uint8_t>> pages_;
  uint32_t next_slot_ = 0;
  uint32_t frees_ = 0;
};

TEST_F(BumpAllocatorUnittest, ReleaseIfDrained) {
  RefPtr<FakePagedMemory> memory(
      new FakePagedMemory(2 * BumpAllocator::kChunkSize));
  BumpAllocator allocator(RefPtr<ChunkMemory>(memory.get()));
  std::vector<BumpAllocator::AllocId> ids;
  for (uint32_t i = 0; i < 3; ++i) {
    ids.push_back(allocator.Alloc(BumpAllocator::kChunkSize));
  }
  void* last_ptr;
  ids.push_back(allocator.Alloc(8, &last_ptr));
  // Chunks 1 and 2 were paged out as chunks 2 and 3 were added.
  ASSERT_EQ(memory->live_pages(), 2u);

  // An undrained chunk keeps its memory.
  RefPtr<TraceBlob> view = allocator.GetAllocation(ids[0]).chunk_blob();
  allocator.ReleaseIfDrained(0);
  ASSERT_FALSE(view->HasOneRef());

  // A drained resident chunk gives up its memory: only the view holds it.
  allocator.Free(ids[0]);
  allocator.ReleaseIfDrained(0);
  ASSERT_TRUE(view->HasOneRef());

  // A drained paged out chunk frees its slot, once.
  allocator.Free(ids[1]);
  allocator.ReleaseIfDrained(1);
  allocator.ReleaseIfDrained(1);
  ASSERT_EQ(memory->frees(), 1u);
  ASSERT_EQ(memory->live_pages(), 1u);

  // The last chunk is still allocated from, so it keeps its memory.
  allocator.Free(ids[3]);
  allocator.ReleaseIfDrained(3);
  void* ptr;
  BumpAllocator::AllocId next = allocator.Alloc(8, &ptr);
  ASSERT_EQ(next.chunk_index, 3u);
  ASSERT_EQ(next.chunk_offset, 8u);
  ASSERT_EQ(static_cast<uint8_t*>(ptr), static_cast<uint8_t*>(last_ptr) + 8);
  memset(ptr, 0xab, 8);
  allocator.Free(next);

  // Erasing the released chunks does not free a slot again; Clear() frees
  // the slot of the still paged out chunk 2.
  ASSERT_EQ(allocator.EraseFrontFreeChunks(), 2u);
  ASSERT_EQ(memory->frees(), 1u);
  allocator.Clear();
  ASSERT_EQ(memory->frees(), 2u);
  ASSERT_EQ(memory->live_pages(), 0u);

  BumpAllocator::AllocId after_clear = allocator.Alloc(8, &ptr);
  ASSERT_EQ(after_clear.chunk_offset, 0u);
  memset(ptr, 0xab, 8);
  allocator.Free(after_clear);
}

}  // namespace perfetto::trace_processor
