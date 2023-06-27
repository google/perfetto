/*
 * Copyright (C) 2023 The Android Open Source Project
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
#include "src/trace_processor/db/storage/numeric_storage.h"

#include "src/trace_processor/db/storage/types.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace storage {
namespace {

using Range = RowMap::Range;

TEST(NumericStorageUnittest, StableSortTrivial) {
  std::vector<uint32_t> data_vec{0, 1, 2, 0, 1, 2, 0, 1, 2};
  std::vector<uint32_t> out = {0, 1, 2, 3, 4, 5, 6, 7, 8};

  NumericStorage storage(data_vec.data(), 9, ColumnType::kUint32);
  RowMap rm(0, 9);
  storage.StableSort(out.data(), 9);

  std::vector<uint32_t> stable_out{0, 3, 6, 1, 4, 7, 2, 5, 8};
  ASSERT_EQ(out, stable_out);
}

TEST(NumericStorageUnittest, StableSort) {
  std::vector<uint32_t> data_vec{0, 1, 2, 0, 1, 2, 0, 1, 2};
  std::vector<uint32_t> out = {1, 7, 4, 0, 6, 3, 2, 5, 8};

  NumericStorage storage(data_vec.data(), 9, ColumnType::kUint32);
  RowMap rm(0, 9);
  storage.StableSort(out.data(), 9);

  std::vector<uint32_t> stable_out{0, 6, 3, 1, 7, 4, 2, 5, 8};
  ASSERT_EQ(out, stable_out);
}

TEST(NumericStorageUnittest, CompareFast) {
  std::vector<uint32_t> data_vec(128);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage storage(data_vec.data(), 128, ColumnType::kUint32);
  RangeOrBitVector range_or_bv =
      storage.Search(FilterOp::kGe, SqlValue::Long(100), Range(0, 128));
  BitVector bv = std::move(range_or_bv).TakeIfBitVector();
  ASSERT_TRUE(range_or_bv.IsBitVector());
  ASSERT_EQ(bv.CountSetBits(), 28u);
  ASSERT_EQ(bv.IndexOfNthSet(0), 100u);
}

TEST(NumericStorageUnittest, CompareSorted) {
  std::vector<uint32_t> data_vec(128);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage storage(data_vec.data(), 128, ColumnType::kUint32, true);
  Range range =
      storage.Search(FilterOp::kGe, SqlValue::Long(100), Range(0, 128))
          .TakeIfRange();
  ASSERT_EQ(range.size(), 28u);
  ASSERT_EQ(range.start, 100u);
  ASSERT_EQ(range.end, 128u);
}

TEST(NumericStorageUnittest, CompareSortedSubset) {
  std::vector<uint32_t> data_vec(128);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage storage(data_vec.data(), 128, ColumnType::kUint32, true);
  Range range =
      storage.Search(FilterOp::kGe, SqlValue::Long(100), Range(102, 104))
          .TakeIfRange();
  ASSERT_EQ(range.size(), 2u);
  ASSERT_EQ(range.start, 102u);
  ASSERT_EQ(range.end, 104u);
}

TEST(NumericStorageUnittest, CompareSortedIndexesGreaterEqual) {
  std::vector<uint32_t> data_vec{30, 40, 50, 60, 90, 80, 70, 0, 10, 20};
  std::vector<uint32_t> sorted_order{7, 8, 9, 0, 1, 2, 3, 6, 5, 4};

  NumericStorage storage(data_vec.data(), 10, ColumnType::kUint32);

  Range range = storage
                    .IndexSearch(FilterOp::kGe, SqlValue::Long(60),
                                 sorted_order.data(), 10, true)
                    .TakeIfRange();

  ASSERT_EQ(range.size(), 4u);
  ASSERT_EQ(range.start, 6u);
  ASSERT_EQ(range.end, 10u);
}

TEST(NumericStorageUnittest, CompareSortedIndexesLess) {
  std::vector<uint32_t> data_vec{30, 40, 50, 60, 90, 80, 70, 0, 10, 20};
  std::vector<uint32_t> sorted_order{7, 8, 9, 0, 1, 2, 3, 6, 5, 4};

  NumericStorage storage(data_vec.data(), 10, ColumnType::kUint32);

  Range range = storage
                    .IndexSearch(FilterOp::kLt, SqlValue::Long(60),
                                 sorted_order.data(), 10, true)
                    .TakeIfRange();

  ASSERT_EQ(range.size(), 6u);
  ASSERT_EQ(range.start, 0u);
  ASSERT_EQ(range.end, 6u);
}

TEST(NumericStorageUnittest, CompareSortedIndexesEqual) {
  std::vector<uint32_t> data_vec{30, 40, 50, 60, 90, 80, 70, 0, 10, 20};
  std::vector<uint32_t> sorted_order{7, 8, 9, 0, 1, 2, 3, 6, 5, 4};

  NumericStorage storage(data_vec.data(), 10, ColumnType::kUint32);

  Range range = storage
                    .IndexSearch(FilterOp::kEq, SqlValue::Long(60),
                                 sorted_order.data(), 10, true)
                    .TakeIfRange();

  ASSERT_EQ(range.size(), 1u);
  ASSERT_EQ(range.start, 6u);
  ASSERT_EQ(range.end, 7u);
}

}  // namespace
}  // namespace storage
}  // namespace trace_processor
}  // namespace perfetto
