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
#include "src/trace_processor/trace_sorter_queue.h"
#include "gtest/gtest.h"
#include "src/trace_processor/types/variadic.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace trace_sorter_internal {

#if PERFETTO_DCHECK_IS_ON()
constexpr uint32_t RESERVED_SIZE_BYTES = 8ul;
#else
constexpr uint32_t RESERVED_SIZE_BYTES = 0ul;
#endif

using ::testing::_;

TEST(VariadicQueueUnittest, AddAndEvict) {
  VariadicQueue queue =
      VariadicQueue::VariadicQueueForTesting(8 + RESERVED_SIZE_BYTES);
  auto offset = queue.Append<int64_t>(10);
  int64_t evicted_val = queue.Evict<int64_t>(offset);
  ASSERT_EQ(evicted_val, 10l);
}

TEST(VariadicQueueUnittest, AddAndEvictFirstElement) {
  VariadicQueue queue =
      VariadicQueue::VariadicQueueForTesting(8 + RESERVED_SIZE_BYTES);
  auto offset1 = queue.Append<int64_t>(10);
  auto offset2 = queue.Append<int64_t>(20);
  ASSERT_EQ(queue.Evict<int64_t>(offset1), 10);
  ASSERT_EQ(queue.Evict<int64_t>(offset2), 20);
}

TEST(VariadicQueueUnittest, AppendAfterEviction) {
  VariadicQueue queue =
      VariadicQueue::VariadicQueueForTesting(8 + RESERVED_SIZE_BYTES);
  auto offset = queue.Append<int64_t>(10);
  ASSERT_EQ(queue.Evict<int64_t>(offset), 10);
  queue.Append<int64_t>(20);
}

TEST(VariadicQueueUnittest, FreeAllMemory) {
  VariadicQueue queue =
      VariadicQueue::VariadicQueueForTesting(8 + RESERVED_SIZE_BYTES);
  auto offset1 = queue.Append<int64_t>(10);
  auto offset2 = queue.Append<int64_t>(20);
  ASSERT_EQ(queue.Evict<int64_t>(offset1), 10);
  ASSERT_EQ(queue.Evict<int64_t>(offset2), 20);
  queue.FreeMemory();
}

TEST(VariadicQueueUnittest, FreeMemoryPartially) {
  VariadicQueue queue =
      VariadicQueue::VariadicQueueForTesting(8 + RESERVED_SIZE_BYTES);
  auto offset1 = queue.Append<int64_t>(10);
  queue.Append<int64_t>(20);
  ASSERT_EQ(queue.Evict<int64_t>(offset1), 10);
  queue.FreeMemory();
}

TEST(VariadicQueueUnittest, AppendDifferentSizes) {
  VariadicQueue queue =
      VariadicQueue::VariadicQueueForTesting(8 + RESERVED_SIZE_BYTES);
  auto offset_long_long = queue.Append<int64_t>(10);
  auto offset_int = queue.Append<int32_t>(20);
  auto offset_short = queue.Append<int16_t>(30);
  auto offset_char = queue.Append<char>('s');
  ASSERT_EQ(queue.Evict<int64_t>(offset_long_long), 10l);
  ASSERT_EQ(queue.Evict<int32_t>(offset_int), 20);
  ASSERT_EQ(queue.Evict<int16_t>(offset_short), static_cast<int16_t>(30));
  ASSERT_EQ(queue.Evict<char>(offset_char), 's');
}

}  // namespace trace_sorter_internal
}  // namespace trace_processor
}  // namespace perfetto
