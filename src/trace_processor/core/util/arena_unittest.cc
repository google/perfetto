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

#include "src/trace_processor/core/util/arena.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core {
namespace {

struct Pair {
  uint32_t a;
  uint64_t b;
};

TEST(ArenaTest, NewMakesAlignedValues) {
  Arena arena;
  for (uint32_t i = 0; i < 1000; ++i) {
    auto* byte = arena.New<uint8_t>(static_cast<uint8_t>(i));
    Pair* pair = arena.New<Pair>(Pair{i, uint64_t{i} * 3});
    EXPECT_EQ(*byte, static_cast<uint8_t>(i));
    EXPECT_EQ(reinterpret_cast<uintptr_t>(pair) % alignof(Pair), 0u);
    EXPECT_EQ(pair->a, i);
    EXPECT_EQ(pair->b, uint64_t{i} * 3);
  }
}

TEST(ArenaTest, SpansLargerThanABlock) {
  Arena arena;
  std::vector<uint64_t> values(10000);
  for (uint32_t i = 0; i < values.size(); ++i) {
    values[i] = i * 7;
  }
  ArenaSpan<uint64_t> copy =
      arena.CopySpan<uint64_t>({values.data(), values.data() + values.size()});
  ArenaSpan<uint32_t> small = arena.NewSpan<uint32_t>(3);
  small[0] = 1;
  small[2] = 3;
  EXPECT_EQ(std::vector<uint64_t>(copy.begin(), copy.end()), values);
  EXPECT_EQ(small.size(), 3u);
  EXPECT_EQ(small[2], 3u);
  small.Shrink(1);
  EXPECT_EQ(small.size(), 1u);
}

TEST(ArenaTest, EmptySpan) {
  Arena arena;
  ArenaSpan<uint32_t> empty = arena.CopySpan<uint32_t>({});
  EXPECT_TRUE(empty.empty());
  EXPECT_EQ(empty.begin(), nullptr);
}

TEST(ArenaTest, MovePreservesPointers) {
  Arena a;
  auto* x = a.New<uint64_t>(uint64_t{42});
  Arena b(std::move(a));
  EXPECT_EQ(*x, 42u);
  auto* y = b.New<uint64_t>(uint64_t{99});
  Arena c;
  c = std::move(b);
  EXPECT_EQ(*x, 42u);
  EXPECT_EQ(*y, 99u);
}

TEST(ArenaTest, OversizedPreservesActiveBlock) {
  Arena arena;
  auto* a = arena.New<uint64_t>(uint64_t{1});
  ArenaSpan<uint64_t> big = arena.NewSpan<uint64_t>(2000);
  big[0] = 7;
  big[1999] = 8;
  auto* b = arena.New<uint64_t>(uint64_t{2});
  // `b` should come from the same initial block right after `a`.
  EXPECT_EQ(b, a + 1);
  EXPECT_EQ(big[0], 7u);
  EXPECT_EQ(big[1999], 8u);
}

TEST(ArenaTest, RemoveIf) {
  Arena arena;
  std::vector<uint32_t> values = {1, 2, 3, 4, 5};
  ArenaSpan<uint32_t> span =
      arena.CopySpan<uint32_t>({values.data(), values.data() + values.size()});
  span.RemoveIf([](uint32_t v) { return v % 2 == 0; });
  EXPECT_THAT(std::vector<uint32_t>(span.begin(), span.end()),
              testing::ElementsAre(1u, 3u, 5u));
}

TEST(ArenaTest, CopiesListsAndStrings) {
  Arena arena;
  ArenaSpan<uint32_t> list = arena.CopySpan<uint32_t>({4, 5, 6});
  EXPECT_EQ(std::vector<uint32_t>(list.begin(), list.end()),
            std::vector<uint32_t>({4, 5, 6}));
  std::string text = "a string longer than any inline buffer";
  std::string_view copy = arena.CopyString(text);
  EXPECT_NE(copy.data(), text.data());
  EXPECT_EQ(copy, text);
  EXPECT_TRUE(arena.CopyString("").empty());
}

}  // namespace
}  // namespace perfetto::trace_processor::core
