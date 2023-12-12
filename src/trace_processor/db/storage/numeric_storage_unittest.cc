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
#include <cstdint>

#include "src/trace_processor/db/storage/types.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {

inline bool operator==(const RowMap::Range& a, const RowMap::Range& b) {
  return std::tie(a.start, a.end) == std::tie(b.start, b.end);
}

namespace storage {
namespace {

using testing::ElementsAre;
using testing::IsEmpty;
using Range = RowMap::Range;

std::vector<uint32_t> ToIndexVector(RangeOrBitVector& r_or_bv) {
  RowMap rm;
  if (r_or_bv.IsBitVector()) {
    rm = RowMap(std::move(r_or_bv).TakeIfBitVector());
  } else {
    Range range = std::move(r_or_bv).TakeIfRange();
    rm = RowMap(range.start, range.end);
  }
  return rm.GetAllIndices();
}

TEST(NumericStorageUnittest, InvalidSearchConstraintsGeneralChecks) {
  std::vector<uint32_t> data_vec(128);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage<uint32_t> storage(&data_vec, ColumnType::kUint32);

  Range test_range(20, 100);
  Range full_range(0, 100);
  Range empty_range;

  // NULL checks
  ASSERT_EQ(storage.ValidateSearchConstraints(SqlValue(), FilterOp::kIsNull),
            SearchValidationResult::kNoData);
  ASSERT_EQ(storage.ValidateSearchConstraints(SqlValue(), FilterOp::kIsNotNull),
            SearchValidationResult::kAllData);

  // FilterOp checks
  ASSERT_EQ(
      storage.ValidateSearchConstraints(SqlValue::Long(15), FilterOp::kGlob),
      SearchValidationResult::kNoData);
  ASSERT_EQ(
      storage.ValidateSearchConstraints(SqlValue::Long(15), FilterOp::kRegex),
      SearchValidationResult::kNoData);

  // Type checks
  ASSERT_EQ(storage.ValidateSearchConstraints(SqlValue::String("cheese"),
                                              FilterOp::kGe),
            SearchValidationResult::kNoData);
}

TEST(NumericStorageUnittest, InvalidValueBoundsUint32) {
  std::vector<uint32_t> data_vec(128);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage<uint32_t> storage(&data_vec, ColumnType::kUint32);

  SqlValue max_val = SqlValue::Long(
      static_cast<int64_t>(std::numeric_limits<uint32_t>::max()) + 10);
  ASSERT_EQ(storage.ValidateSearchConstraints(max_val, FilterOp::kGe),
            SearchValidationResult::kNoData);
  ASSERT_EQ(storage.ValidateSearchConstraints(max_val, FilterOp::kGt),
            SearchValidationResult::kNoData);
  ASSERT_EQ(storage.ValidateSearchConstraints(max_val, FilterOp::kEq),
            SearchValidationResult::kNoData);

  ASSERT_EQ(storage.ValidateSearchConstraints(max_val, FilterOp::kLe),
            SearchValidationResult::kAllData);
  ASSERT_EQ(storage.ValidateSearchConstraints(max_val, FilterOp::kLt),
            SearchValidationResult::kAllData);
  ASSERT_EQ(storage.ValidateSearchConstraints(max_val, FilterOp::kNe),
            SearchValidationResult::kAllData);

  SqlValue min_val = SqlValue::Long(
      static_cast<int64_t>(std::numeric_limits<uint32_t>::min()) - 1);
  ASSERT_EQ(storage.ValidateSearchConstraints(min_val, FilterOp::kGe),
            SearchValidationResult::kAllData);
  ASSERT_EQ(storage.ValidateSearchConstraints(min_val, FilterOp::kGt),
            SearchValidationResult::kAllData);
  ASSERT_EQ(storage.ValidateSearchConstraints(min_val, FilterOp::kNe),
            SearchValidationResult::kAllData);

  ASSERT_EQ(storage.ValidateSearchConstraints(min_val, FilterOp::kLe),
            SearchValidationResult::kNoData);
  ASSERT_EQ(storage.ValidateSearchConstraints(min_val, FilterOp::kLt),
            SearchValidationResult::kNoData);
  ASSERT_EQ(storage.ValidateSearchConstraints(min_val, FilterOp::kEq),
            SearchValidationResult::kNoData);
}

TEST(NumericStorageUnittest, InvalidValueBoundsInt32) {
  std::vector<int32_t> data_vec(128);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage<int32_t> storage(&data_vec, ColumnType::kInt32);

  SqlValue max_val = SqlValue::Long(
      static_cast<int64_t>(std::numeric_limits<int32_t>::max()) + 10);
  ASSERT_EQ(storage.ValidateSearchConstraints(max_val, FilterOp::kGe),
            SearchValidationResult::kNoData);
  ASSERT_EQ(storage.ValidateSearchConstraints(max_val, FilterOp::kGt),
            SearchValidationResult::kNoData);
  ASSERT_EQ(storage.ValidateSearchConstraints(max_val, FilterOp::kEq),
            SearchValidationResult::kNoData);

  ASSERT_EQ(storage.ValidateSearchConstraints(max_val, FilterOp::kLe),
            SearchValidationResult::kAllData);
  ASSERT_EQ(storage.ValidateSearchConstraints(max_val, FilterOp::kLt),
            SearchValidationResult::kAllData);
  ASSERT_EQ(storage.ValidateSearchConstraints(max_val, FilterOp::kNe),
            SearchValidationResult::kAllData);

  SqlValue min_val = SqlValue::Long(
      static_cast<int64_t>(std::numeric_limits<int32_t>::min()) - 1);
  ASSERT_EQ(storage.ValidateSearchConstraints(min_val, FilterOp::kGe),
            SearchValidationResult::kAllData);
  ASSERT_EQ(storage.ValidateSearchConstraints(min_val, FilterOp::kGt),
            SearchValidationResult::kAllData);
  ASSERT_EQ(storage.ValidateSearchConstraints(min_val, FilterOp::kNe),
            SearchValidationResult::kAllData);

  ASSERT_EQ(storage.ValidateSearchConstraints(min_val, FilterOp::kLe),
            SearchValidationResult::kNoData);
  ASSERT_EQ(storage.ValidateSearchConstraints(min_val, FilterOp::kLt),
            SearchValidationResult::kNoData);
  ASSERT_EQ(storage.ValidateSearchConstraints(min_val, FilterOp::kEq),
            SearchValidationResult::kNoData);
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

TEST(NumericStorageUnittest, Search) {
  std::vector<int32_t> data_vec{-5, 5, -4, 4, -3, 3, 0};
  NumericStorage<int32_t> storage(&data_vec, ColumnType::kInt32);
  Range test_range(1, 5);
  SqlValue val = SqlValue::Long(4);

  auto res = storage.Search(FilterOp::kEq, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(3));

  res = storage.Search(FilterOp::kNe, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(1, 2, 4));

  res = storage.Search(FilterOp::kLt, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(2, 4));

  res = storage.Search(FilterOp::kLe, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(2, 3, 4));

  res = storage.Search(FilterOp::kGt, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(1));

  res = storage.Search(FilterOp::kGe, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(1, 3));
}

TEST(NumericStorageUnittest, SearchCompareWithNegative) {
  std::vector<int32_t> data_vec{-5, 5, -4, 4, -3, 3, 0};
  NumericStorage<int32_t> storage(&data_vec, ColumnType::kInt32);
  Range test_range(1, 5);
  SqlValue val = SqlValue::Long(-3);

  auto res = storage.Search(FilterOp::kEq, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(4));

  res = storage.Search(FilterOp::kNe, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(1, 2, 3));

  res = storage.Search(FilterOp::kLt, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(2));

  res = storage.Search(FilterOp::kLe, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(2, 4));

  res = storage.Search(FilterOp::kGt, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(1, 3));

  res = storage.Search(FilterOp::kGe, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(1, 3, 4));
}

TEST(NumericStorageUnittest, IndexSearch) {
  std::vector<int32_t> data_vec{-5, 5, -4, 4, -3, 3, 0};
  NumericStorage<int32_t> storage(&data_vec, ColumnType::kInt32);

  // -5, -3, -3, 3, 5, 0
  std::vector<uint32_t> indices{0, 4, 4, 5, 1, 6};
  SqlValue val = SqlValue::Long(3);

  auto res = storage.IndexSearch(FilterOp::kEq, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(3));

  res = storage.IndexSearch(FilterOp::kNe, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(0, 1, 2, 4, 5));

  res = storage.IndexSearch(FilterOp::kLt, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(0, 1, 2, 5));

  res = storage.IndexSearch(FilterOp::kLe, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(0, 1, 2, 3, 5));

  res = storage.IndexSearch(FilterOp::kGt, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(4));

  res = storage.IndexSearch(FilterOp::kGe, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(3, 4));
}

TEST(NumericStorageUnittest, IndexSearchCompareWithNegative) {
  std::vector<int32_t> data_vec{-5, 5, -4, 4, -3, 3, 0};
  NumericStorage<int32_t> storage(&data_vec, ColumnType::kInt32);

  // -5, -3, -3, 3, 5, 0
  std::vector<uint32_t> indices{0, 4, 4, 5, 1, 6};
  SqlValue val = SqlValue::Long(-3);

  auto res = storage.IndexSearch(FilterOp::kEq, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(1, 2));

  res = storage.IndexSearch(FilterOp::kNe, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(0, 3, 4, 5));

  res = storage.IndexSearch(FilterOp::kLt, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(0));

  res = storage.IndexSearch(FilterOp::kLe, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(0, 1, 2));

  res = storage.IndexSearch(FilterOp::kGt, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(3, 4, 5));

  res = storage.IndexSearch(FilterOp::kGe, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(1, 2, 3, 4, 5));
}

TEST(NumericStorageUnittest, SearchFast) {
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

TEST(NumericStorageUnittest, SearchSorted) {
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

TEST(NumericStorageUnittest, SearchSortedNe) {
  std::vector<uint32_t> data_vec(128);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage<uint32_t> storage(&data_vec, ColumnType::kUint32, true);
  BitVector bv =
      storage.Search(FilterOp::kNe, SqlValue::Long(100), Range(0, 128))
          .TakeIfBitVector();
  ASSERT_EQ(bv.CountSetBits(), 127u);
}

TEST(NumericStorageUnittest, SearchSortedSubset) {
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

TEST(NumericStorageUnittest, IndexSearcgExtrinsicGe) {
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

TEST(NumericStorageUnittest, IndexSearchExtrinsicLt) {
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

TEST(NumericStorageUnittest, IndexSearchExtrinsicEq) {
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

TEST(NumericStorageUnittest, SearchWithIntAsDouble) {
  std::vector<int32_t> data_vec{-5, 5, -4, 4, -3, 3, 0};
  NumericStorage<int32_t> storage(&data_vec, ColumnType::kInt32);
  Range test_range(1, 5);
  SqlValue val = SqlValue::Double(4);

  auto res = storage.Search(FilterOp::kEq, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(3));

  res = storage.Search(FilterOp::kNe, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(1, 2, 4));

  res = storage.Search(FilterOp::kLt, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(2, 4));

  res = storage.Search(FilterOp::kLe, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(2, 3, 4));

  res = storage.Search(FilterOp::kGt, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(1));

  res = storage.Search(FilterOp::kGe, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(1, 3));
}

TEST(NumericStorageUnittest, IndexSearchWithIntAsDouble) {
  std::vector<int32_t> data_vec{-5, 5, -4, 4, -3, 3, 0};
  NumericStorage<int32_t> storage(&data_vec, ColumnType::kInt32);

  // -5, -3, -3, 3, 5, 0
  std::vector<uint32_t> indices{0, 4, 4, 5, 1, 6};
  SqlValue val = SqlValue::Double(3);

  auto res = storage.IndexSearch(FilterOp::kEq, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(3));

  res = storage.IndexSearch(FilterOp::kNe, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(0, 1, 2, 4, 5));

  res = storage.IndexSearch(FilterOp::kLt, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(0, 1, 2, 5));

  res = storage.IndexSearch(FilterOp::kLe, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(0, 1, 2, 3, 5));

  res = storage.IndexSearch(FilterOp::kGt, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(4));

  res = storage.IndexSearch(FilterOp::kGe, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(3, 4));
}

TEST(NumericStorageUnittest, SearchInt32WithDouble) {
  std::vector<int32_t> data_vec{-5, 5, -4, 4, -3, 3, 0};
  NumericStorage<int32_t> storage(&data_vec, ColumnType::kInt32);
  Range test_range(1, 5);
  SqlValue val = SqlValue::Double(3.5);

  auto res = storage.Search(FilterOp::kEq, val, test_range);
  ASSERT_THAT(ToIndexVector(res), IsEmpty());

  res = storage.Search(FilterOp::kNe, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(1, 2, 3, 4));

  res = storage.Search(FilterOp::kLt, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(2, 4));

  res = storage.Search(FilterOp::kLe, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(2, 4));

  res = storage.Search(FilterOp::kGt, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(1, 3));

  res = storage.Search(FilterOp::kGe, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(1, 3));
}

TEST(NumericStorageUnittest, SearchInt32WithNegDouble) {
  std::vector<int32_t> data_vec{-5, 5, -4, 4, -3, 3, 0};
  NumericStorage<int32_t> storage(&data_vec, ColumnType::kInt32);
  Range test_range(1, 5);
  SqlValue val = SqlValue::Double(-3.5);

  auto res = storage.Search(FilterOp::kEq, val, test_range);
  ASSERT_THAT(ToIndexVector(res), IsEmpty());

  res = storage.Search(FilterOp::kNe, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(1, 2, 3, 4));

  res = storage.Search(FilterOp::kLt, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(2));

  res = storage.Search(FilterOp::kLe, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(2));

  res = storage.Search(FilterOp::kGt, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(1, 3, 4));

  res = storage.Search(FilterOp::kGe, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(1, 3, 4));
}

TEST(NumericStorageUnittest, IndexSearchInt32WithDouble) {
  std::vector<int32_t> data_vec{-5, 5, -4, 4, -3, 3, 0};
  NumericStorage<int32_t> storage(&data_vec, ColumnType::kInt32);

  // -5, -3, -3, 3, 5, 0
  std::vector<uint32_t> indices{0, 4, 4, 5, 1, 6};
  SqlValue val = SqlValue::Double(1.5);

  auto res = storage.IndexSearch(FilterOp::kEq, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), IsEmpty());

  res = storage.IndexSearch(FilterOp::kNe, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(0, 1, 2, 3, 4, 5));

  res = storage.IndexSearch(FilterOp::kLt, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(0, 1, 2, 5));

  res = storage.IndexSearch(FilterOp::kLe, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(0, 1, 2, 5));

  res = storage.IndexSearch(FilterOp::kGt, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(3, 4));

  res = storage.IndexSearch(FilterOp::kGe, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(3, 4));
}

TEST(NumericStorageUnittest, IndexSearchInt32WithNegDouble) {
  std::vector<int32_t> data_vec{-5, 5, -4, 4, -3, 3, 0};
  NumericStorage<int32_t> storage(&data_vec, ColumnType::kInt32);

  // -5, -3, -3, 3, 5, 0
  std::vector<uint32_t> indices{0, 4, 4, 5, 1, 6};
  SqlValue val = SqlValue::Double(-2.5);

  auto res = storage.IndexSearch(FilterOp::kEq, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), IsEmpty());

  res = storage.IndexSearch(FilterOp::kNe, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(0, 1, 2, 3, 4, 5));

  res = storage.IndexSearch(FilterOp::kLt, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(0, 1, 2));

  res = storage.IndexSearch(FilterOp::kLe, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(0, 1, 2));

  res = storage.IndexSearch(FilterOp::kGt, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(3, 4, 5));

  res = storage.IndexSearch(FilterOp::kGe, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(3, 4, 5));
}

TEST(NumericStorageUnittest, SearchUint32WithNegDouble) {
  std::vector<uint32_t> data_vec{0, 1, 2, 3, 4, 5};
  NumericStorage<uint32_t> storage(&data_vec, ColumnType::kInt32);
  Range test_range(1, 5);
  SqlValue val = SqlValue::Double(-3.5);

  auto res = storage.Search(FilterOp::kEq, val, test_range);
  ASSERT_THAT(ToIndexVector(res), IsEmpty());

  res = storage.Search(FilterOp::kNe, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(1, 2, 3, 4));

  res = storage.Search(FilterOp::kLt, val, test_range);
  ASSERT_THAT(ToIndexVector(res), IsEmpty());

  res = storage.Search(FilterOp::kLe, val, test_range);
  ASSERT_THAT(ToIndexVector(res), IsEmpty());

  res = storage.Search(FilterOp::kGt, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(1, 2, 3, 4));

  res = storage.Search(FilterOp::kGe, val, test_range);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(1, 2, 3, 4));
}

TEST(NumericStorageUnittest, IndexSearchUint32WithNegDouble) {
  std::vector<uint32_t> data_vec{0, 1, 2, 3, 4, 5, 6};
  NumericStorage<uint32_t> storage(&data_vec, ColumnType::kInt32);

  std::vector<uint32_t> indices{0, 4, 4, 5, 1, 6};
  SqlValue val = SqlValue::Double(-2.5);

  auto res = storage.IndexSearch(FilterOp::kEq, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), IsEmpty());

  res = storage.IndexSearch(FilterOp::kNe, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(0, 1, 2, 3, 4, 5));

  res = storage.IndexSearch(FilterOp::kLt, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), IsEmpty());

  res = storage.IndexSearch(FilterOp::kLe, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), IsEmpty());

  res = storage.IndexSearch(FilterOp::kGt, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(0, 1, 2, 3, 4, 5));

  res = storage.IndexSearch(FilterOp::kGe, val, indices.data(), 6, false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(0, 1, 2, 3, 4, 5));
}

}  // namespace
}  // namespace storage
}  // namespace trace_processor
}  // namespace perfetto
