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

#include "src/tracing/service/block_arena.h"

#include <string.h>

#include <string>
#include <utility>
#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace internal {
namespace {

TEST(BlockArenaTest, BlockIsReusedAfterItsLastAllocation) {
  BlockArena arena(100);
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 0u);

  auto a = arena.Allocate(60);  // Block 0.
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 100u);
  auto b = arena.Allocate(60);  // Block 1 because it does not fit after a.
  auto c = arena.Allocate(40);  // Appends to block 1, after b.
  EXPECT_EQ(c.data(), b.data() + 60);
  EXPECT_NE(b.data(), a.data());
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 200u);
  memset(a.data(), 'a', 60);
  memset(b.data(), 'b', 60);
  memset(c.data(), 'c', 40);
  const uint8_t* block_0 = a.data();

  // Block 0 starts over from its first byte, so all 100 bytes fit.
  a = {};
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 200u);
  auto reused = arena.Allocate(100);
  EXPECT_EQ(reused.data(), block_0);
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 200u);
  memset(reused.data(), 'd', 100);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(c.data()), 40),
            std::string(40, 'c'));
}

TEST(BlockArenaTest, BlockStaysUntilItsLastAllocationIsDestroyed) {
  BlockArena arena(100);
  auto first = arena.Allocate(60);
  auto second = arena.Allocate(40);
  ASSERT_EQ(second.data(), first.data() + 60);
  memset(second.data(), 'a', 40);
  const uint8_t* block = first.data();

  // |second| still uses the block, so the next allocation gets a new block.
  first = {};
  auto other = arena.Allocate(100);
  EXPECT_NE(other.data(), block);
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 200u);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(second.data()), 40),
            std::string(40, 'a'));

  second = {};
  auto reused = arena.Allocate(100);
  EXPECT_EQ(reused.data(), block);
}

TEST(BlockArenaTest, OlderBlockTakesAllocationsOnceNewestIsReleased) {
  BlockArena arena(100);
  auto older = arena.Allocate(60);   // Block 0.
  auto newest = arena.Allocate(60);  // Block 1 because it does not fit.
  ASSERT_NE(newest.data(), older.data() + 60);

  // Block 1 goes to the free blocks, so block 0 is the last active block again.
  newest = {};
  auto next = arena.Allocate(40);
  EXPECT_EQ(next.data(), older.data() + 60);
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 200u);
  memset(next.data(), 'a', 40);
}

TEST(BlockArenaTest, MovesReleaseReplacedAllocation) {
  BlockArena arena(100);
  BlockArena::Allocation live;
  {
    auto first = arena.Allocate(300);
    memset(first.data(), 'a', 300);
    auto moved = std::move(first);

    live = arena.Allocate(400);
    EXPECT_EQ(arena.GetMemoryUsageBytes(), 300u + 400);

    // Replacement destroys the 400-byte allocation, which frees its block.
    live = std::move(moved);
    EXPECT_EQ(arena.GetMemoryUsageBytes(), 300u);
  }

  // Neither moved-from owner holds the 300-byte allocation.
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 300u);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(live.data()), 300),
            std::string(300, 'a'));

  live = {};
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 0u);
}

TEST(BlockArenaTest, OversizedRequestSkipsFreeBlocks) {
  BlockArena arena(100);
  auto a = arena.Allocate(100);
  const uint8_t* block = a.data();
  a = {};
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 100u);

  // The free block is too small, so this request gets its own block.
  auto oversized = arena.Allocate(150);
  EXPECT_NE(oversized.data(), block);
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 250u);
  memset(oversized.data(), 'a', 150);

  // Oversized blocks aren't cached, so only the free block remains.
  oversized = {};
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 100u);
}

TEST(BlockArenaTest, FreeBlocksAreBounded) {
  BlockArena arena(100);
  std::vector<BlockArena::Allocation> live;
  for (int i = 0; i < 4; ++i) {
    live.push_back(arena.Allocate(100));
    memset(live.back().data(), 'a' + i, 100);
  }
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 400u);

  // The first two blocks to lose their last allocation are cached.
  live.pop_back();
  live.pop_back();
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 400u);

  // The cache is full, so the next block is freed.
  live.pop_back();
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 300u);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(live[0].data()), 100),
            std::string(100, 'a'));
}

}  // namespace
}  // namespace internal
}  // namespace perfetto
