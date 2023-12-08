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

inline bool operator==(const RowMap::Range& a, const RowMap::Range& b) {
  return std::tie(a.start, a.end) == std::tie(b.start, b.end);
}

namespace storage {
namespace {

using Range = RowMap::Range;

TEST(NumericStorageUnittest, InvalidSearchConstraintsGeneralChecks) {
  std::vector<uint32_t> data_vec(128);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage<uint32_t> storage(&data_vec, ColumnType::kUint32);

  Range test_range(20, 100);
  Range full_range(0, 100);
  Range empty_range;

  // NULL checks
  SqlValue val;
  val.type = SqlValue::kNull;
  Range search_result =
      storage.Search(FilterOp::kIsNull, val, test_range).TakeIfRange();
  ASSERT_EQ(search_result, empty_range);
  search_result =
      storage.Search(FilterOp::kIsNotNull, val, test_range).TakeIfRange();
  ASSERT_EQ(search_result, full_range);

  // FilterOp checks
  search_result =
      storage.Search(FilterOp::kGlob, SqlValue::Long(15), test_range)
          .TakeIfRange();
  ASSERT_EQ(search_result, empty_range);
  search_result =
      storage.Search(FilterOp::kRegex, SqlValue::Long(15), test_range)
          .TakeIfRange();
  ASSERT_EQ(search_result, empty_range);

  // Type checks
  search_result =
      storage.Search(FilterOp::kGe, SqlValue::String("cheese"), test_range)
          .TakeIfRange();
  ASSERT_EQ(search_result, empty_range);
}

TEST(NumericStorageUnittest, InvalidValueBoundsUint32) {
  std::vector<uint32_t> data_vec(128);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage<uint32_t> storage(&data_vec, ColumnType::kUint32);

  Range test_range(20, 100);
  Range full_range(0, 100);
  Range empty_range;

  SqlValue max_val = SqlValue::Long(
      static_cast<int64_t>(std::numeric_limits<uint32_t>::max()) + 10);
  Range search_result =
      storage.Search(FilterOp::kGe, max_val, test_range).TakeIfRange();
  ASSERT_EQ(search_result, empty_range);
  search_result =
      storage.Search(FilterOp::kGt, max_val, test_range).TakeIfRange();
  ASSERT_EQ(search_result, empty_range);
  search_result =
      storage.Search(FilterOp::kEq, max_val, test_range).TakeIfRange();
  ASSERT_EQ(search_result, empty_range);

  search_result =
      storage.Search(FilterOp::kLe, max_val, test_range).TakeIfRange();
  ASSERT_EQ(search_result, full_range);
  search_result =
      storage.Search(FilterOp::kLt, max_val, test_range).TakeIfRange();
  ASSERT_EQ(search_result, full_range);
  search_result =
      storage.Search(FilterOp::kNe, max_val, test_range).TakeIfRange();
  ASSERT_EQ(search_result, full_range);

  SqlValue min_val = SqlValue::Long(
      static_cast<int64_t>(std::numeric_limits<uint32_t>::min()) - 1);
  search_result =
      storage.Search(FilterOp::kGe, min_val, test_range).TakeIfRange();
  ASSERT_EQ(search_result, full_range);
  search_result =
      storage.Search(FilterOp::kGt, min_val, test_range).TakeIfRange();
  ASSERT_EQ(search_result, full_range);
  search_result =
      storage.Search(FilterOp::kNe, min_val, test_range).TakeIfRange();
  ASSERT_EQ(search_result, full_range);

  search_result =
      storage.Search(FilterOp::kLe, min_val, test_range).TakeIfRange();
  ASSERT_EQ(search_result, empty_range);
  search_result =
      storage.Search(FilterOp::kLt, min_val, test_range).TakeIfRange();
  ASSERT_EQ(search_result, empty_range);
  search_result =
      storage.Search(FilterOp::kEq, min_val, test_range).TakeIfRange();
  ASSERT_EQ(search_result, empty_range);
}

TEST(NumericStorageUnittest, InvalidValueBoundsInt32) {
  std::vector<int32_t> data_vec(128);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage<int32_t> storage(&data_vec, ColumnType::kInt32);

  Range test_range(20, 100);
  Range full_range(0, 100);
  Range empty_range;

  SqlValue max_val = SqlValue::Long(
      static_cast<int64_t>(std::numeric_limits<int32_t>::max()) + 10);
  Range search_result =
      storage.Search(FilterOp::kGe, max_val, test_range).TakeIfRange();
  ASSERT_EQ(search_result, empty_range);
  search_result =
      storage.Search(FilterOp::kGt, max_val, test_range).TakeIfRange();
  ASSERT_EQ(search_result, empty_range);
  search_result =
      storage.Search(FilterOp::kEq, max_val, test_range).TakeIfRange();
  ASSERT_EQ(search_result, empty_range);

  search_result =
      storage.Search(FilterOp::kLe, max_val, test_range).TakeIfRange();
  ASSERT_EQ(search_result, full_range);
  search_result =
      storage.Search(FilterOp::kLt, max_val, test_range).TakeIfRange();
  ASSERT_EQ(search_result, full_range);
  search_result =
      storage.Search(FilterOp::kNe, max_val, test_range).TakeIfRange();
  ASSERT_EQ(search_result, full_range);

  SqlValue min_val = SqlValue::Long(
      static_cast<int64_t>(std::numeric_limits<int32_t>::min()) - 1);
  search_result =
      storage.Search(FilterOp::kGe, min_val, test_range).TakeIfRange();
  ASSERT_EQ(search_result, full_range);
  search_result =
      storage.Search(FilterOp::kGt, min_val, test_range).TakeIfRange();
  ASSERT_EQ(search_result, full_range);
  search_result =
      storage.Search(FilterOp::kNe, min_val, test_range).TakeIfRange();
  ASSERT_EQ(search_result, full_range);

  search_result =
      storage.Search(FilterOp::kLe, min_val, test_range).TakeIfRange();
  ASSERT_EQ(search_result, empty_range);
  search_result =
      storage.Search(FilterOp::kLt, min_val, test_range).TakeIfRange();
  ASSERT_EQ(search_result, empty_range);
  search_result =
      storage.Search(FilterOp::kEq, min_val, test_range).TakeIfRange();
  ASSERT_EQ(search_result, empty_range);
}

TEST(NumericStorageUnittest, StableSortTrivial) {
  std::vector<uint32_t> data_vec{0, 1, 2, 0, 1, 2, 0, 1, 2};
  std::vector<uint32_t> out = {0, 1, 2, 3, 4, 5, 6, 7, 8};

  NumericStorage<uint32_t> storage(&data_vec, ColumnType::kUint32);
  RowMap rm(0, 9);
  storage.StableSort(out.data(), 9);

  std::vector<uint32_t> stable_out{0, 3, 6, 1, 4, 7, 2, 5, 8};
  ASSERT_EQ(out, stable_out);
}

TEST(NumericStorageUnittest, StableSort) {
  std::vector<uint32_t> data_vec{0, 1, 2, 0, 1, 2, 0, 1, 2};
  std::vector<uint32_t> out = {1, 7, 4, 0, 6, 3, 2, 5, 8};

  NumericStorage<uint32_t> storage(&data_vec, ColumnType::kUint32);
  RowMap rm(0, 9);
  storage.StableSort(out.data(), 9);

  std::vector<uint32_t> stable_out{0, 6, 3, 1, 7, 4, 2, 5, 8};
  ASSERT_EQ(out, stable_out);
}

TEST(NumericStorageUnittest, CompareFast) {
  std::vector<uint32_t> data_vec(128);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage<uint32_t> storage(&data_vec, ColumnType::kUint32);
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
  NumericStorage<uint32_t> storage(&data_vec, ColumnType::kUint32, true);
  Range range =
      storage.Search(FilterOp::kGe, SqlValue::Long(100), Range(0, 128))
          .TakeIfRange();
  ASSERT_EQ(range.size(), 28u);
  ASSERT_EQ(range.start, 100u);
  ASSERT_EQ(range.end, 128u);
}

TEST(NumericStorageUnittest, CompareSortedNe) {
  std::vector<uint32_t> data_vec(128);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage<uint32_t> storage(&data_vec, ColumnType::kUint32, true);
  BitVector bv =
      storage.Search(FilterOp::kNe, SqlValue::Long(100), Range(0, 128))
          .TakeIfBitVector();
  ASSERT_EQ(bv.CountSetBits(), 127u);
}

TEST(NumericStorageUnittest, CompareSortedSubset) {
  std::vector<uint32_t> data_vec(128);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage<uint32_t> storage(&data_vec, ColumnType::kUint32, true);
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

  NumericStorage<uint32_t> storage(&data_vec, ColumnType::kUint32);

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

  NumericStorage<uint32_t> storage(&data_vec, ColumnType::kUint32);

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

  NumericStorage<uint32_t> storage(&data_vec, ColumnType::kUint32);

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
