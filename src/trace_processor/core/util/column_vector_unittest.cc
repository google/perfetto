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

#include "src/trace_processor/core/util/column_vector.h"

#include <cstdint>
#include <utility>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core {
namespace {

TEST(ColumnVectorTest, OwnedStorageGrowsAndPreservesValues) {
  auto values = ColumnVector<uint64_t>::CreateWithCapacity(64);
  for (uint64_t i = 0; i < 4096; ++i)
    values.push_back(i * 7);
  values.shrink_to_fit();
  ASSERT_EQ(values.size(), 4096u);
  for (uint64_t i = 0; i < values.size(); ++i)
    ASSERT_EQ(values[i], i * 7);
}

TEST(ColumnVectorTest, MovePreservesCachedPointerState) {
  auto source = ColumnVector<uint32_t>::CreateFilled(100, 42);
  const uint32_t* data = std::as_const(source).data();
  ColumnVector<uint32_t> destination = std::move(source);
  EXPECT_EQ(std::as_const(destination).data(), data);
  destination.push_back(7);
  EXPECT_EQ(destination[100], 7u);
}

}  // namespace
}  // namespace perfetto::trace_processor::core
