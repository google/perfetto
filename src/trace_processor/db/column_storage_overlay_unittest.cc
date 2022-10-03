/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "src/trace_processor/db/column_storage_overlay.h"

#include <memory>

#include "src/base/test/gtest_test_suite.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

TEST(ColumnStorageOverlay, FilterIntoEmptyOutput) {
  ColumnStorageOverlay rm(0, 10000);
  RowMap filter(4, 4);
  rm.FilterInto(&filter, [](uint32_t) -> bool {
    ADD_FAILURE() << "Should not have called lambda";
    return true;
  });

  ASSERT_EQ(filter.size(), 0u);
}

TEST(ColumnStorageOverlay, FilterIntoSingleRowTrue) {
  ColumnStorageOverlay rm(100, 10000);
  RowMap filter(6, 7);
  rm.FilterInto(&filter, [](uint32_t row) { return row == 106u; });

  ASSERT_EQ(filter.size(), 1u);
  ASSERT_EQ(filter.Get(0u), 6u);
}

TEST(ColumnStorageOverlay, FilterIntoSingleRowFalse) {
  ColumnStorageOverlay rm(100, 10000);
  RowMap filter(6, 7);
  rm.FilterInto(&filter, [](uint32_t row) {
    EXPECT_EQ(row, 106u);
    return row != 106u;
  });

  ASSERT_EQ(filter.size(), 0u);
}

TEST(ColumnStorageOverlay, FilterIntoRangeWithRange) {
  ColumnStorageOverlay rm(93, 157);
  RowMap filter(4, 7);
  rm.FilterInto(&filter, [](uint32_t row) { return row == 97u || row == 98u; });

  ASSERT_EQ(filter.size(), 2u);
  ASSERT_EQ(filter.Get(0u), 4u);
  ASSERT_EQ(filter.Get(1u), 5u);
}

TEST(ColumnStorageOverlay, FilterIntoOffsetRangeWithRange) {
  ColumnStorageOverlay rm(100000, 100010);
  RowMap filter(4, 7);
  rm.FilterInto(&filter, [](uint32_t row) { return row == 100004u; });

  ASSERT_EQ(filter.size(), 1u);
  ASSERT_EQ(filter.Get(0u), 4u);
}

TEST(ColumnStorageOverlay, FilterIntoLargeRangeWithRange) {
  ColumnStorageOverlay rm(0, 100000);
  RowMap filter(0, 100000);
  rm.FilterInto(&filter, [](uint32_t row) { return row % 2 == 0; });

  ASSERT_EQ(filter.size(), 100000u / 2);
  for (uint32_t i = 0; i < 100000 / 2; ++i) {
    ASSERT_EQ(filter.Get(i), i * 2);
  }
}

TEST(ColumnStorageOverlay, FilterIntoBitVectorWithRange) {
  ColumnStorageOverlay rm(
      BitVector{true, false, false, true, false, true, false, true, true});
  RowMap filter(1u, 5u);
  rm.FilterInto(&filter, [](uint32_t row) { return row == 3u || row == 7u; });

  ASSERT_EQ(filter.size(), 2u);
  ASSERT_EQ(filter.Get(0u), 1u);
  ASSERT_EQ(filter.Get(1u), 3u);
}

TEST(ColumnStorageOverlay, FilterIntoIndexVectorWithRange) {
  ColumnStorageOverlay rm(std::vector<uint32_t>{33, 2u, 45u, 7u, 8u, 9u});
  RowMap filter(2, 5);
  rm.FilterInto(&filter, [](uint32_t row) { return row == 45u || row == 8u; });

  ASSERT_EQ(filter.size(), 2u);
  ASSERT_EQ(filter.Get(0u), 2u);
  ASSERT_EQ(filter.Get(1u), 4u);
}

TEST(ColumnStorageOverlay, FilterIntoRangeWithBitVector) {
  ColumnStorageOverlay rm(27, 31);
  RowMap filter(BitVector{true, false, true, true});
  rm.FilterInto(&filter, [](uint32_t row) { return row == 29u || row == 30u; });

  ASSERT_EQ(filter.size(), 2u);
  ASSERT_EQ(filter.Get(0u), 2u);
  ASSERT_EQ(filter.Get(1u), 3u);
}

TEST(ColumnStorageOverlay, FilterIntoBitVectorWithBitVector) {
  ColumnStorageOverlay rm(BitVector{true, false, true, true, false, true});
  RowMap filter(BitVector{true, true, false, true});
  rm.FilterInto(&filter, [](uint32_t row) { return row == 2u || row == 5u; });

  ASSERT_EQ(filter.size(), 2u);
  ASSERT_EQ(filter.Get(0u), 1u);
  ASSERT_EQ(filter.Get(1u), 3u);
}

TEST(ColumnStorageOverlay, FilterIntoIndexVectorWithBitVector) {
  ColumnStorageOverlay rm(std::vector<uint32_t>{0u, 2u, 3u, 5u});
  RowMap filter(BitVector{true, true, false, true});
  rm.FilterInto(&filter, [](uint32_t row) { return row == 2u || row == 5u; });

  ASSERT_EQ(filter.size(), 2u);
  ASSERT_EQ(filter.Get(0u), 1u);
  ASSERT_EQ(filter.Get(1u), 3u);
}

TEST(ColumnStorageOverlay, FilterIntoRangeWithIndexVector) {
  ColumnStorageOverlay rm(27, 41);
  RowMap filter(std::vector<uint32_t>{3u, 5u, 9u, 10u, 12u});
  rm.FilterInto(&filter, [](uint32_t row) { return row == 32u || row == 39u; });

  ASSERT_EQ(filter.size(), 2u);
  ASSERT_EQ(filter.Get(0u), 5u);
  ASSERT_EQ(filter.Get(1u), 12u);
}

TEST(ColumnStorageOverlay, FilterIntoBitVectorWithIndexVector) {
  ColumnStorageOverlay rm(
      BitVector{false, true, false, true, true, false, true});
  RowMap filter(std::vector<uint32_t>{1u, 2u, 3u});
  rm.FilterInto(&filter, [](uint32_t row) { return row == 3u || row == 4u; });

  ASSERT_EQ(filter.size(), 2u);
  ASSERT_EQ(filter.Get(0u), 1u);
  ASSERT_EQ(filter.Get(1u), 2u);
}

TEST(ColumnStorageOverlay, FilterIntoIndexVectorWithIndexVector) {
  ColumnStorageOverlay rm(std::vector<uint32_t>{33u, 2u, 45u, 7u, 8u, 9u});
  RowMap filter(std::vector<uint32_t>{1u, 2u, 3u});
  rm.FilterInto(&filter, [](uint32_t row) { return row == 2u || row == 7u; });

  ASSERT_EQ(filter.size(), 2u);
  ASSERT_EQ(filter.Get(0u), 1u);
  ASSERT_EQ(filter.Get(1u), 3u);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
