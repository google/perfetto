/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/profiling/memory/bookkeeping.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace perfetto {
namespace profiling {
namespace {

using ::testing::AnyOf;
using ::testing::Eq;

std::vector<FrameData> stack() {
  std::vector<FrameData> res;

  unwindstack::FrameData data{};
  data.function_name = "fun1";
  data.map_name = "map1";
  res.emplace_back(std::move(data), "dummy_buildid");
  data = {};
  data.function_name = "fun2";
  data.map_name = "map2";
  res.emplace_back(std::move(data), "dummy_buildid");
  return res;
}

std::vector<FrameData> stack2() {
  std::vector<FrameData> res;
  unwindstack::FrameData data{};
  data.function_name = "fun1";
  data.map_name = "map1";
  res.emplace_back(std::move(data), "dummy_buildid");
  data = {};
  data.function_name = "fun3";
  data.map_name = "map3";
  res.emplace_back(std::move(data), "dummy_buildid");
  return res;
}

TEST(BookkeepingTest, Basic) {
  uint64_t sequence_number = 1;
  GlobalCallstackTrie c;
  HeapTracker hd(&c);

  hd.RecordMalloc(stack(), 1, 5, sequence_number, 100 * sequence_number);
  sequence_number++;
  hd.RecordMalloc(stack2(), 2, 2, sequence_number, 100 * sequence_number);
  sequence_number++;
  ASSERT_EQ(hd.GetSizeForTesting(stack()), 5);
  ASSERT_EQ(hd.GetSizeForTesting(stack2()), 2);
  ASSERT_EQ(hd.GetTimestampForTesting(), 100 * (sequence_number - 1));
  hd.RecordFree(2, sequence_number, 100 * sequence_number);
  sequence_number++;
  ASSERT_EQ(hd.GetSizeForTesting(stack()), 5);
  ASSERT_EQ(hd.GetSizeForTesting(stack2()), 0);
  ASSERT_EQ(hd.GetTimestampForTesting(), 100 * (sequence_number - 1));
  hd.RecordFree(1, sequence_number, 100 * sequence_number);
  sequence_number++;
  ASSERT_EQ(hd.GetSizeForTesting(stack()), 0);
  ASSERT_EQ(hd.GetSizeForTesting(stack2()), 0);
  ASSERT_EQ(hd.GetTimestampForTesting(), 100 * (sequence_number - 1));
}

TEST(BookkeepingTest, TwoHeapTrackers) {
  uint64_t sequence_number = 1;
  GlobalCallstackTrie c;
  HeapTracker hd(&c);
  {
    HeapTracker hd2(&c);

    hd.RecordMalloc(stack(), 1, 5, sequence_number, 100 * sequence_number);
    hd2.RecordMalloc(stack(), 2, 2, sequence_number, 100 * sequence_number);
    sequence_number++;
    ASSERT_EQ(hd2.GetSizeForTesting(stack()), 2);
    ASSERT_EQ(hd.GetSizeForTesting(stack()), 5);
    ASSERT_EQ(hd.GetTimestampForTesting(), 100 * (sequence_number - 1));
  }
  ASSERT_EQ(hd.GetSizeForTesting(stack()), 5);
}

TEST(BookkeepingTest, ReplaceAlloc) {
  uint64_t sequence_number = 1;
  GlobalCallstackTrie c;
  HeapTracker hd(&c);

  hd.RecordMalloc(stack(), 1, 5, sequence_number, 100 * sequence_number);
  sequence_number++;
  hd.RecordMalloc(stack2(), 1, 2, sequence_number, 100 * sequence_number);
  sequence_number++;
  EXPECT_EQ(hd.GetSizeForTesting(stack()), 0);
  EXPECT_EQ(hd.GetSizeForTesting(stack2()), 2);
  ASSERT_EQ(hd.GetTimestampForTesting(), 100 * (sequence_number - 1));
}

TEST(BookkeepingTest, OutOfOrder) {
  GlobalCallstackTrie c;
  HeapTracker hd(&c);

  hd.RecordMalloc(stack(), 1, 5, 2, 2);
  hd.RecordMalloc(stack2(), 1, 2, 1, 1);
  EXPECT_EQ(hd.GetSizeForTesting(stack()), 5);
  EXPECT_EQ(hd.GetSizeForTesting(stack2()), 0);
}

TEST(BookkeepingTest, ManyAllocations) {
  GlobalCallstackTrie c;
  HeapTracker hd(&c);

  std::vector<std::pair<uint64_t, uint64_t>> batch_frees;

  for (uint64_t sequence_number = 1; sequence_number < 1000;) {
    if (batch_frees.size() > 10) {
      for (const auto& p : batch_frees)
        hd.RecordFree(p.first, p.second, 100 * p.second);
      batch_frees.clear();
    }

    uint64_t addr = sequence_number;
    hd.RecordMalloc(stack(), addr, 5, sequence_number, sequence_number);
    sequence_number++;
    batch_frees.emplace_back(addr, sequence_number++);
    ASSERT_THAT(hd.GetSizeForTesting(stack()), AnyOf(Eq(0), Eq(5)));
  }
}

TEST(BookkeepingTest, ArbitraryOrder) {
  std::vector<FrameData> s = stack();
  std::vector<FrameData> s2 = stack2();

  struct Operation {
    uint64_t sequence_number;
    uint64_t address;
    uint64_t bytes;                       // 0 for free
    const std::vector<FrameData>* stack;  // nullptr for free

    // For std::next_permutation.
    bool operator<(const Operation& other) const {
      return sequence_number < other.sequence_number;
    }
  } operations[] = {
      {1, 1, 5, &s},       //
      {2, 1, 10, &s2},     //
      {3, 1, 0, nullptr},  //
      {4, 2, 0, nullptr},  //
      {5, 3, 0, nullptr},  //
      {6, 3, 2, &s},       //
      {7, 4, 3, &s2},      //
  };

  uint64_t s_size = 2;
  uint64_t s2_size = 3;

  do {
    GlobalCallstackTrie c;
    HeapTracker hd(&c);

    for (auto it = std::begin(operations); it != std::end(operations); ++it) {
      const Operation& operation = *it;

      if (operation.bytes == 0) {
        hd.RecordFree(operation.address, operation.sequence_number,
                      100 * operation.sequence_number);
      } else {
        hd.RecordMalloc(*operation.stack, operation.address, operation.bytes,
                        operation.sequence_number,
                        100 * operation.sequence_number);
      }
    }
    ASSERT_EQ(hd.GetSizeForTesting(s), s_size);
    ASSERT_EQ(hd.GetSizeForTesting(s2), s2_size);
  } while (std::next_permutation(std::begin(operations), std::end(operations)));
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
