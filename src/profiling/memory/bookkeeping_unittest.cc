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

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace profiling {
namespace {

using ::testing::AnyOf;
using ::testing::Eq;

std::vector<UnwindFrame> stack() {
  std::vector<UnwindFrame> res;

  UnwindFrame frame{};
  frame.function_name = "fun1";
  frame.pc = 1;
  frame.build_id = "dummy_buildid";
  res.emplace_back(std::move(frame));
  frame = {};
  frame.function_name = "fun2";
  frame.pc = 2;
  frame.build_id = "dummy_buildid";
  res.emplace_back(std::move(frame));
  return res;
}

std::vector<UnwindFrame> stack2() {
  std::vector<UnwindFrame> res;
  UnwindFrame frame{};
  frame.function_name = "fun1";
  frame.pc = 1;
  frame.build_id = "dummy_buildid";
  res.emplace_back(std::move(frame));
  frame = {};
  frame.function_name = "fun3";
  frame.pc = 3;
  frame.build_id = "dummy_buildid";
  res.emplace_back(std::move(frame));
  return res;
}

std::vector<UnwindFrame> stack3() {
  std::vector<UnwindFrame> res;
  UnwindFrame frame{};
  frame.function_name = "fun1";
  frame.pc = 1;
  frame.build_id = "dummy_buildid";
  res.emplace_back(std::move(frame));
  frame = {};
  frame.function_name = "fun4";
  frame.pc = 4;
  frame.build_id = "dummy_buildid";
  res.emplace_back(std::move(frame));
  return res;
}

TEST(BookkeepingTest, EmptyStack) {
  uint64_t sequence_number = 1;
  GlobalCallstackTrie c;
  HeapTracker hd(&c, false);

  hd.RecordMalloc({}, 0x1, 5, 5, sequence_number, 100 * sequence_number);
  sequence_number++;
  hd.RecordFree(0x1, sequence_number, 100 * sequence_number);
  hd.GetCallstackAllocations([](const HeapTracker::CallstackAllocations&) {});
}

TEST(BookkeepingTest, Replace) {
  uint64_t sequence_number = 1;
  GlobalCallstackTrie c;
  HeapTracker hd(&c, false);

  hd.RecordMalloc(stack(), 1, 5, 5, sequence_number, 100 * sequence_number);
  sequence_number++;
  hd.RecordMalloc(stack2(), 1, 2, 2, sequence_number, 100 * sequence_number);

  // Call GetCallstackAllocations twice to force GC of old CallstackAllocations.
  hd.GetCallstackAllocations([](const HeapTracker::CallstackAllocations&) {});
  hd.GetCallstackAllocations([](const HeapTracker::CallstackAllocations&) {});
}

TEST(BookkeepingTest, Basic) {
  uint64_t sequence_number = 1;
  GlobalCallstackTrie c;
  HeapTracker hd(&c, false);

  hd.RecordMalloc(stack(), 0x1, 5, 5, sequence_number, 100 * sequence_number);
  sequence_number++;
  hd.RecordMalloc(stack2(), 0x2, 2, 2, sequence_number, 100 * sequence_number);
  sequence_number++;
  ASSERT_EQ(hd.GetSizeForTesting(stack()), 5u);
  ASSERT_EQ(hd.GetSizeForTesting(stack2()), 2u);
  ASSERT_EQ(hd.GetTimestampForTesting(), 100 * (sequence_number - 1));
  hd.RecordFree(0x2, sequence_number, 100 * sequence_number);
  sequence_number++;
  ASSERT_EQ(hd.GetSizeForTesting(stack()), 5u);
  ASSERT_EQ(hd.GetSizeForTesting(stack2()), 0u);
  ASSERT_EQ(hd.GetTimestampForTesting(), 100 * (sequence_number - 1));
  hd.RecordFree(0x1, sequence_number, 100 * sequence_number);
  sequence_number++;
  ASSERT_EQ(hd.GetSizeForTesting(stack()), 0u);
  ASSERT_EQ(hd.GetSizeForTesting(stack2()), 0u);
  ASSERT_EQ(hd.GetTimestampForTesting(), 100 * (sequence_number - 1));
}

TEST(BookkeepingTest, Max) {
  uint64_t sequence_number = 1;
  GlobalCallstackTrie c;
  HeapTracker hd(&c, true);

  hd.RecordMalloc(stack(), 0x1, 5, 5, sequence_number, 100 * sequence_number);
  sequence_number++;
  hd.RecordMalloc(stack2(), 0x2, 2, 2, sequence_number, 100 * sequence_number);
  sequence_number++;
  hd.RecordFree(0x2, sequence_number, 100 * sequence_number);
  sequence_number++;
  hd.RecordFree(0x1, sequence_number, 100 * sequence_number);
  sequence_number++;
  hd.RecordMalloc(stack2(), 0x2, 1, 2, sequence_number, 100 * sequence_number);
  ASSERT_EQ(hd.dump_timestamp(), 200u);
  ASSERT_EQ(hd.GetMaxForTesting(stack()), 5u);
  ASSERT_EQ(hd.GetMaxForTesting(stack2()), 2u);
  ASSERT_EQ(hd.GetMaxCountForTesting(stack()), 1u);
  ASSERT_EQ(hd.GetMaxCountForTesting(stack2()), 1u);
}

TEST(BookkeepingTest, Max2) {
  uint64_t sequence_number = 1;
  GlobalCallstackTrie c;
  HeapTracker hd(&c, true);

  hd.RecordMalloc(stack(), 0x1, 10u, 10u, sequence_number,
                  100 * sequence_number);
  sequence_number++;
  hd.RecordFree(0x1, sequence_number, 100 * sequence_number);
  sequence_number++;
  hd.RecordMalloc(stack2(), 0x2, 15u, 15u, sequence_number,
                  100 * sequence_number);
  sequence_number++;
  hd.RecordMalloc(stack3(), 0x3, 15u, 15u, sequence_number,
                  100 * sequence_number);
  sequence_number++;
  hd.RecordFree(0x2, sequence_number, 100 * sequence_number);
  EXPECT_EQ(hd.dump_timestamp(), 400u);
  EXPECT_EQ(hd.GetMaxForTesting(stack()), 0u);
  EXPECT_EQ(hd.GetMaxForTesting(stack2()), 15u);
  EXPECT_EQ(hd.GetMaxForTesting(stack3()), 15u);
  EXPECT_EQ(hd.GetMaxCountForTesting(stack()), 0u);
  EXPECT_EQ(hd.GetMaxCountForTesting(stack2()), 1u);
  EXPECT_EQ(hd.GetMaxCountForTesting(stack3()), 1u);
}

TEST(BookkeepingTest, TwoHeapTrackers) {
  uint64_t sequence_number = 1;
  GlobalCallstackTrie c;
  HeapTracker hd(&c, false);
  {
    HeapTracker hd2(&c, false);

    hd.RecordMalloc(stack(), 0x1, 5, 5, sequence_number, 100 * sequence_number);
    hd2.RecordMalloc(stack(), 0x2, 2, 2, sequence_number,
                     100 * sequence_number);
    sequence_number++;
    ASSERT_EQ(hd2.GetSizeForTesting(stack()), 2u);
    ASSERT_EQ(hd.GetSizeForTesting(stack()), 5u);
    ASSERT_EQ(hd.GetTimestampForTesting(), 100 * (sequence_number - 1));
  }
  ASSERT_EQ(hd.GetSizeForTesting(stack()), 5u);
}

TEST(BookkeepingTest, ReplaceAlloc) {
  uint64_t sequence_number = 1;
  GlobalCallstackTrie c;
  HeapTracker hd(&c, false);

  hd.RecordMalloc(stack(), 0x1, 5, 5, sequence_number, 100 * sequence_number);
  sequence_number++;
  hd.RecordMalloc(stack2(), 0x1, 2, 2, sequence_number, 100 * sequence_number);
  sequence_number++;
  EXPECT_EQ(hd.GetSizeForTesting(stack()), 0u);
  EXPECT_EQ(hd.GetSizeForTesting(stack2()), 2u);
  ASSERT_EQ(hd.GetTimestampForTesting(), 100 * (sequence_number - 1));
}

TEST(BookkeepingTest, OutOfOrder) {
  GlobalCallstackTrie c;
  HeapTracker hd(&c, false);

  hd.RecordMalloc(stack(), 0x1, 5, 5, 2, 2);
  hd.RecordMalloc(stack2(), 0x1, 2, 2, 1, 1);
  EXPECT_EQ(hd.GetSizeForTesting(stack()), 5u);
  EXPECT_EQ(hd.GetSizeForTesting(stack2()), 0u);
}

TEST(BookkeepingTest, ManyAllocations) {
  GlobalCallstackTrie c;
  HeapTracker hd(&c, false);

  std::vector<std::pair<uint64_t, uint64_t>> batch_frees;

  for (uint64_t sequence_number = 1; sequence_number < 1000;) {
    if (batch_frees.size() > 10) {
      for (const auto& p : batch_frees)
        hd.RecordFree(p.first, p.second, 100 * p.second);
      batch_frees.clear();
    }

    uint64_t addr = sequence_number;
    hd.RecordMalloc(stack(), addr, 5, 5, sequence_number, sequence_number);
    sequence_number++;
    batch_frees.emplace_back(addr, sequence_number++);
    ASSERT_THAT(hd.GetSizeForTesting(stack()), AnyOf(Eq(0u), Eq(5u)));
  }
}

TEST(BookkeepingTest, ArbitraryOrder) {
  std::vector<UnwindFrame> s = stack();
  std::vector<UnwindFrame> s2 = stack2();

  enum OperationType { kAlloc, kFree, kDump };

  struct Operation {
    uint64_t sequence_number;
    OperationType type;
    uint64_t address;
    uint64_t bytes;                         // 0 for free
    const std::vector<UnwindFrame>* stack;  // nullptr for free

    // For std::next_permutation.
    bool operator<(const Operation& other) const {
      return sequence_number < other.sequence_number;
    }
  } operations[] = {
      {1, kAlloc, 0x1, 5, &s},       //
      {2, kAlloc, 0x1, 10, &s2},     //
      {3, kFree, 0x01, 0, nullptr},  //
      {4, kFree, 0x02, 0, nullptr},  //
      {5, kFree, 0x03, 0, nullptr},  //
      {6, kAlloc, 0x3, 2, &s},       //
      {7, kAlloc, 0x4, 3, &s2},      //
      {0, kDump, 0x00, 0, nullptr},  //
  };

  uint64_t s_size = 2;
  uint64_t s2_size = 3;

  do {
    GlobalCallstackTrie c;
    HeapTracker hd(&c, false);

    for (auto it = std::begin(operations); it != std::end(operations); ++it) {
      const Operation& operation = *it;

      if (operation.type == OperationType::kFree) {
        hd.RecordFree(operation.address, operation.sequence_number,
                      100 * operation.sequence_number);
      } else if (operation.type == OperationType::kAlloc) {
        hd.RecordMalloc(*operation.stack, operation.address, operation.bytes,
                        operation.bytes, operation.sequence_number,
                        100 * operation.sequence_number);
      } else {
        hd.GetCallstackAllocations(
            [](const HeapTracker::CallstackAllocations&) {});
      }
    }
    ASSERT_EQ(hd.GetSizeForTesting(s), s_size);
    ASSERT_EQ(hd.GetSizeForTesting(s2), s2_size);
  } while (std::next_permutation(std::begin(operations), std::end(operations)));
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
