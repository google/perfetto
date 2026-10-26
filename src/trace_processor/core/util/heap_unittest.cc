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

#include "src/trace_processor/core/util/heap.h"

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core {
namespace {

constexpr auto kLater = [](uint32_t a, uint32_t b) { return a > b; };

void Push(std::vector<uint32_t>& heap, uint32_t value) {
  heap.push_back(value);
  HeapSiftUp(heap.data(), heap.size() - 1, value, kLater);
}

uint32_t Pop(std::vector<uint32_t>& heap) {
  uint32_t top = heap[0];
  uint32_t last = heap.back();
  heap.pop_back();
  if (!heap.empty()) {
    HeapSiftDown(heap.data(), heap.size(), last, kLater);
  }
  return top;
}

// Pushes, pops and replaces the top at random, against a sorted multiset,
// checking that each pop is the smallest and that the layout stays a heap as
// std::is_heap() sees it.
TEST(HeapTest, MatchesSortedOrder) {
  std::minstd_rand0 rnd(0);
  std::vector<uint32_t> heap;
  std::vector<uint32_t> expected;
  for (uint32_t i = 0; i < 20000; ++i) {
    uint32_t op = rnd() % 3;
    if (op == 0 || expected.empty()) {
      uint32_t value = rnd() % 100;
      Push(heap, value);
      expected.insert(std::upper_bound(expected.begin(), expected.end(), value),
                      value);
    } else if (op == 1) {
      ASSERT_EQ(Pop(heap), expected.front());
      expected.erase(expected.begin());
    } else {
      // Replace the top with a later value, as a merge does.
      uint32_t value = heap[0] + rnd() % 10;
      HeapSiftDown(heap.data(), heap.size(), value, kLater);
      expected.erase(expected.begin());
      expected.insert(std::upper_bound(expected.begin(), expected.end(), value),
                      value);
    }
    ASSERT_TRUE(std::is_heap(heap.begin(), heap.end(), kLater));
  }
}

}  // namespace
}  // namespace perfetto::trace_processor::core
