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

std::vector<unwindstack::FrameData> stack() {
  std::vector<unwindstack::FrameData> res;
  unwindstack::FrameData data{};
  data.function_name = "fun1";
  data.map_name = "map1";
  res.emplace_back(std::move(data));
  data = {};
  data.function_name = "fun2";
  data.map_name = "map2";
  res.emplace_back(std::move(data));
  return res;
}

std::vector<unwindstack::FrameData> stack2() {
  std::vector<unwindstack::FrameData> res;
  unwindstack::FrameData data{};
  data.function_name = "fun1";
  data.map_name = "map1";
  res.emplace_back(std::move(data));
  data = {};
  data.function_name = "fun3";
  data.map_name = "map3";
  res.emplace_back(std::move(data));
  return res;
}

TEST(BookkeepingTest, Basic) {
  uint64_t sequence_number = 1;
  GlobalCallstackTrie c;
  HeapTracker hd(&c);

  hd.RecordMalloc(stack(), 1, 5, sequence_number++);
  hd.RecordMalloc(stack2(), 2, 2, sequence_number++);
  ASSERT_EQ(hd.GetSizeForTesting(stack()), 5);
  ASSERT_EQ(hd.GetSizeForTesting(stack2()), 2);
  hd.RecordFree(2, sequence_number++);
  ASSERT_EQ(hd.GetSizeForTesting(stack()), 5);
  ASSERT_EQ(hd.GetSizeForTesting(stack2()), 0);
  hd.RecordFree(1, sequence_number++);
  ASSERT_EQ(hd.GetSizeForTesting(stack()), 0);
  ASSERT_EQ(hd.GetSizeForTesting(stack2()), 0);
}

TEST(BookkeepingTest, TwoHeapTrackers) {
  uint64_t sequence_number = 1;
  GlobalCallstackTrie c;
  HeapTracker hd(&c);
  {
    HeapTracker hd2(&c);

    hd.RecordMalloc(stack(), 1, 5, sequence_number++);
    hd2.RecordMalloc(stack(), 2, 2, sequence_number++);
    ASSERT_EQ(hd2.GetSizeForTesting(stack()), 2);
    ASSERT_EQ(hd.GetSizeForTesting(stack()), 5);
  }
  ASSERT_EQ(hd.GetSizeForTesting(stack()), 5);
}

TEST(BookkeepingTest, ReplaceAlloc) {
  uint64_t sequence_number = 1;
  GlobalCallstackTrie c;
  HeapTracker hd(&c);

  hd.RecordMalloc(stack(), 1, 5, sequence_number++);
  hd.RecordMalloc(stack2(), 1, 2, sequence_number++);
  EXPECT_EQ(hd.GetSizeForTesting(stack()), 0);
  EXPECT_EQ(hd.GetSizeForTesting(stack2()), 2);
}

TEST(BookkeepingTest, OutOfOrder) {
  GlobalCallstackTrie c;
  HeapTracker hd(&c);

  hd.RecordMalloc(stack(), 1, 5, 1);
  hd.RecordMalloc(stack2(), 1, 2, 0);
  EXPECT_EQ(hd.GetSizeForTesting(stack()), 5);
  EXPECT_EQ(hd.GetSizeForTesting(stack2()), 0);
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
