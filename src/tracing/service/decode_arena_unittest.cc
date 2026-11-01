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

#include "src/tracing/service/decode_arena.h"

#include <string.h>

#include <string>
#include <utility>
#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace internal {
namespace {

TEST(DecodeArenaTest, BlocksAreReusedAfterReset) {
  DecodeArena arena(100);
  const size_t two_blocks = 200;
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 0u);

  auto a = arena.Allocate(60);  // Block 0.
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 100u);
  auto b = arena.Allocate(60);  // Block 1 because it does not fit after a.
  auto c = arena.Allocate(40);  // Appends to block 1, after b.
  EXPECT_EQ(c.data(), b.data() + 60);
  EXPECT_NE(b.data(), a.data());
  EXPECT_EQ(arena.GetMemoryUsageBytes(), two_blocks);

  auto d = arena.Allocate(50);  // Another ordinary block.
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 300u);
  memset(a.data(), 'a', 60);
  memset(b.data(), 'b', 60);
  memset(c.data(), 'c', 40);
  memset(d.data(), 'd', 50);
  const uint8_t* block_2 = d.data();

  // Keep c's whole block and one free block, so two blocks stay allocated.
  a.Release();
  b.Release();
  d.Release();
  arena.Reset();
  EXPECT_EQ(arena.GetMemoryUsageBytes(), two_blocks);
  auto reused = arena.Allocate(100);
  EXPECT_EQ(reused.data(), block_2);
  memset(reused.data(), 'e', 100);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(c.data()), 40),
            std::string(40, 'c'));
}

TEST(DecodeArenaTest, CompactionPreservesLiveAllocations) {
  DecodeArena arena(100);
  DecodeArena::Allocation live;
  {
    auto first = arena.Allocate(60);
    live = arena.Allocate(40);
    ASSERT_EQ(live.data(), first.data() + 60);
    memset(live.data(), 'a', 40);
  }
  auto other = arena.Allocate(100);
  memset(other.data(), 'b', 100);

  // Ending the first allocation must not reclaim the block shared with |live|.
  // Both remaining allocations survive, even though they use different blocks.
  for (int i = 0; i < 2; ++i) {
    arena.Reset();
    arena.Compact();
    EXPECT_EQ(std::string(reinterpret_cast<char*>(live.data()), 40),
              std::string(40, 'a'));
    EXPECT_EQ(std::string(reinterpret_cast<char*>(other.data()), 100),
              std::string(100, 'b'));
  }
}

TEST(DecodeArenaTest, MovesReleaseReplacedAllocation) {
  DecodeArena arena(100);
  DecodeArena::Allocation live;
  {
    auto first = arena.Allocate(300);
    memset(first.data(), 'a', 300);
    auto moved = std::move(first);

    live = arena.Allocate(400);
    live = std::move(moved);
    // Replacement releases the 400-byte allocation, but keeps its bytes until
    // Reset() or Compact().
    // Neither moved-from owner holds the 300-byte allocation.
    EXPECT_EQ(arena.GetMemoryUsageBytes(), 300u + 400);
  }

  // Only the oversized allocation in |live| survives Reset() and Compact().
  for (int i = 0; i < 2; ++i) {
    arena.Reset();
    arena.Compact();
    EXPECT_EQ(arena.GetMemoryUsageBytes(), 300u);
    EXPECT_EQ(std::string(reinterpret_cast<char*>(live.data()), 300),
              std::string(300, 'a'));
  }

  live.Release();
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 300u);
  arena.Reset();
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 0u);
  arena.Compact();
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 0u);
}

TEST(DecodeArenaTest, ReleasedBytesSurviveLaterAllocations) {
  DecodeArena arena(100);
  auto first = arena.Allocate(60);
  memset(first.data(), 'a', 60);
  const uint8_t* data = first.data();
  first.Release();

  // Later allocations can append or acquire a block, but cannot rewind bytes
  // that this batch still borrows.
  auto tail = arena.Allocate(40);
  EXPECT_EQ(tail.data(), data + 60);
  memset(tail.data(), 'b', 40);
  auto next = arena.Allocate(100);
  memset(next.data(), 'c', 100);
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(data), 60),
            std::string(60, 'a'));
  EXPECT_EQ(std::string(reinterpret_cast<char*>(tail.data()), 40),
            std::string(40, 'b'));

  tail.Release();
  next.Release();
  arena.Reset();
  auto reused = arena.Allocate(100);
  EXPECT_EQ(reused.data(), data);
}

TEST(DecodeArenaTest, RetainedBlocksAreBounded) {
  DecodeArena arena(100);
  std::vector<DecodeArena::Allocation> live;
  for (int i = 0; i < 4; ++i) {
    live.push_back(arena.Allocate(100));
    memset(live.back().data(), 'a' + i, 100);
  }
  for (int i = 0; i < 4; ++i) {
    auto spare = arena.Allocate(100);
    memset(spare.data(), 's', 100);
  }
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 800u);

  // Live blocks always stay, and they already exceed the retention target.
  arena.Reset();
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 400u);

  // So a block that stops being live is freed instead of kept for reuse.
  live.pop_back();
  arena.Reset();
  arena.Reset();
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 300u);
  arena.Compact();
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 300u);  // Only the three live blocks.
  for (size_t i = 0; i < live.size(); ++i) {
    EXPECT_EQ(std::string(reinterpret_cast<char*>(live[i].data()), 100),
              std::string(100, static_cast<char>('a' + i)));
  }

  // With nothing live, two of the three blocks stay for reuse.
  live.clear();
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 300u);
  arena.Reset();
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 200u);
}

}  // namespace
}  // namespace internal
}  // namespace perfetto
