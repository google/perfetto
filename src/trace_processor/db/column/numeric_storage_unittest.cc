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
#include "src/trace_processor/db/column/numeric_storage.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <tuple>
#include <vector>

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/row_map.h"
#include "src/trace_processor/db/column/data_layer.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/db/column/utils.h"
#include "src/trace_processor/db/compare.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {

inline bool operator==(const Range& a, const Range& b) {
  return std::tie(a.start, a.end) == std::tie(b.start, b.end);
}

namespace column {
namespace {

using testing::ElementsAre;
using testing::IsEmpty;

using Indices = DataLayerChain::Indices;
using OrderedIndices = DataLayerChain::OrderedIndices;

TEST(NumericStorage, InvalidSearchConstraintsGeneralChecks) {
  std::vector<uint32_t> data_vec(128);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage<uint32_t> storage(&data_vec, ColumnType::kUint32, false);
  auto chain = storage.MakeChain();

  Range test_range(20, 100);
  Range full_range(0, 100);
  Range empty_range;

  // NULL checks
  ASSERT_EQ(chain->ValidateSearchConstraints(FilterOp::kIsNull, SqlValue()),
            SearchValidationResult::kNoData);
  ASSERT_EQ(chain->ValidateSearchConstraints(FilterOp::kIsNotNull, SqlValue()),
            SearchValidationResult::kAllData);

  // FilterOp checks
  ASSERT_EQ(
      chain->ValidateSearchConstraints(FilterOp::kGlob, SqlValue::Long(15)),
      SearchValidationResult::kNoData);
  ASSERT_EQ(
      chain->ValidateSearchConstraints(FilterOp::kRegex, SqlValue::Long(15)),
      SearchValidationResult::kNoData);

  // Type checks
  ASSERT_EQ(chain->ValidateSearchConstraints(FilterOp::kGe,
                                             SqlValue::String("cheese")),
            SearchValidationResult::kNoData);
}

TEST(NumericStorage, InvalidValueBoundsUint32) {
  std::vector<uint32_t> data_vec(128);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage<uint32_t> storage(&data_vec, ColumnType::kUint32, false);
  auto chain = storage.MakeChain();

  SqlValue max_val = SqlValue::Long(
      static_cast<int64_t>(std::numeric_limits<uint32_t>::max()) + 10);
  ASSERT_EQ(chain->ValidateSearchConstraints(FilterOp::kGe, max_val),
            SearchValidationResult::kNoData);
  ASSERT_EQ(chain->ValidateSearchConstraints(FilterOp::kGt, max_val),
            SearchValidationResult::kNoData);
  ASSERT_EQ(chain->ValidateSearchConstraints(FilterOp::kEq, max_val),
            SearchValidationResult::kNoData);

  ASSERT_EQ(chain->ValidateSearchConstraints(FilterOp::kLe, max_val),
            SearchValidationResult::kAllData);
  ASSERT_EQ(chain->ValidateSearchConstraints(FilterOp::kLt, max_val),
            SearchValidationResult::kAllData);
  ASSERT_EQ(chain->ValidateSearchConstraints(FilterOp::kNe, max_val),
            SearchValidationResult::kAllData);

  SqlValue min_val = SqlValue::Long(
      static_cast<int64_t>(std::numeric_limits<uint32_t>::min()) - 1);
  ASSERT_EQ(chain->ValidateSearchConstraints(FilterOp::kGe, min_val),
            SearchValidationResult::kAllData);
  ASSERT_EQ(chain->ValidateSearchConstraints(FilterOp::kGt, min_val),
            SearchValidationResult::kAllData);
  ASSERT_EQ(chain->ValidateSearchConstraints(FilterOp::kNe, min_val),
            SearchValidationResult::kAllData);

  ASSERT_EQ(chain->ValidateSearchConstraints(FilterOp::kLe, min_val),
            SearchValidationResult::kNoData);
  ASSERT_EQ(chain->ValidateSearchConstraints(FilterOp::kLt, min_val),
            SearchValidationResult::kNoData);
  ASSERT_EQ(chain->ValidateSearchConstraints(FilterOp::kEq, min_val),
            SearchValidationResult::kNoData);
}

TEST(NumericStorage, InvalidValueBoundsInt32) {
  std::vector<int32_t> data_vec(128);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage<int32_t> storage(&data_vec, ColumnType::kInt32, false);
  auto chain = storage.MakeChain();

  SqlValue max_val = SqlValue::Long(
      static_cast<int64_t>(std::numeric_limits<int32_t>::max()) + 10);
  ASSERT_EQ(chain->ValidateSearchConstraints(FilterOp::kGe, max_val),
            SearchValidationResult::kNoData);
  ASSERT_EQ(chain->ValidateSearchConstraints(FilterOp::kGt, max_val),
            SearchValidationResult::kNoData);
  ASSERT_EQ(chain->ValidateSearchConstraints(FilterOp::kEq, max_val),
            SearchValidationResult::kNoData);

  ASSERT_EQ(chain->ValidateSearchConstraints(FilterOp::kLe, max_val),
            SearchValidationResult::kAllData);
  ASSERT_EQ(chain->ValidateSearchConstraints(FilterOp::kLt, max_val),
            SearchValidationResult::kAllData);
  ASSERT_EQ(chain->ValidateSearchConstraints(FilterOp::kNe, max_val),
            SearchValidationResult::kAllData);

  SqlValue min_val = SqlValue::Long(
      static_cast<int64_t>(std::numeric_limits<int32_t>::min()) - 1);
  ASSERT_EQ(chain->ValidateSearchConstraints(FilterOp::kGe, min_val),
            SearchValidationResult::kAllData);
  ASSERT_EQ(chain->ValidateSearchConstraints(FilterOp::kGt, min_val),
            SearchValidationResult::kAllData);
  ASSERT_EQ(chain->ValidateSearchConstraints(FilterOp::kNe, min_val),
            SearchValidationResult::kAllData);

  ASSERT_EQ(chain->ValidateSearchConstraints(FilterOp::kLe, min_val),
            SearchValidationResult::kNoData);
  ASSERT_EQ(chain->ValidateSearchConstraints(FilterOp::kLt, min_val),
            SearchValidationResult::kNoData);
  ASSERT_EQ(chain->ValidateSearchConstraints(FilterOp::kEq, min_val),
            SearchValidationResult::kNoData);
}

TEST(NumericStorage, SingleSearch) {
  std::vector<int32_t> data_vec{0, 1, 2, 3, 0, -1, -2, -3};
  NumericStorage<int32_t> storage(&data_vec, ColumnType::kInt32, false);
  auto chain = storage.MakeChain();

  ASSERT_EQ(chain->SingleSearch(FilterOp::kEq, SqlValue::Long(1), 1),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kEq, SqlValue::Long(1), 5),
            SingleSearchResult::kNoMatch);

  ASSERT_EQ(chain->SingleSearch(FilterOp::kNe, SqlValue::Long(1), 0),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kNe, SqlValue::Long(-2), 6),
            SingleSearchResult::kNoMatch);

  ASSERT_EQ(chain->SingleSearch(FilterOp::kLt, SqlValue::Long(3), 2),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kLt, SqlValue::Long(-2), 5),
            SingleSearchResult::kNoMatch);

  ASSERT_EQ(chain->SingleSearch(FilterOp::kLe, SqlValue::Long(4), 4),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kLe, SqlValue::Long(0), 3),
            SingleSearchResult::kNoMatch);

  ASSERT_EQ(chain->SingleSearch(FilterOp::kGt, SqlValue::Long(0), 3),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kGt, SqlValue::Long(0), 5),
            SingleSearchResult::kNoMatch);

  ASSERT_EQ(chain->SingleSearch(FilterOp::kGe, SqlValue::Long(0), 0),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kGe, SqlValue::Long(0), 5),
            SingleSearchResult::kNoMatch);

  ASSERT_EQ(chain->SingleSearch(FilterOp::kIsNull, SqlValue(), 0),
            SingleSearchResult::kNoMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kIsNotNull, SqlValue(), 0),
            SingleSearchResult::kMatch);
}

TEST(NumericStorage, Search) {
  std::vector<int32_t> data_vec{-5, 5, -4, 4, -3, 3, 0};
  NumericStorage<int32_t> storage(&data_vec, ColumnType::kInt32, false);
  auto chain = storage.MakeChain();
  Range test_range(1, 5);
  SqlValue val = SqlValue::Long(4);

  auto res = chain->Search(FilterOp::kEq, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3));

  res = chain->Search(FilterOp::kNe, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 2, 4));

  res = chain->Search(FilterOp::kLt, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2, 4));

  res = chain->Search(FilterOp::kLe, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2, 3, 4));

  res = chain->Search(FilterOp::kGt, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1));

  res = chain->Search(FilterOp::kGe, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 3));
}

TEST(NumericStorage, SearchCompareWithNegative) {
  std::vector<int32_t> data_vec{-5, 5, -4, 4, -3, 3, 0};
  NumericStorage<int32_t> storage(&data_vec, ColumnType::kInt32, false);
  auto chain = storage.MakeChain();
  Range test_range(1, 5);
  SqlValue val = SqlValue::Long(-3);

  auto res = chain->Search(FilterOp::kEq, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(4));

  res = chain->Search(FilterOp::kNe, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 2, 3));

  res = chain->Search(FilterOp::kLt, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2));

  res = chain->Search(FilterOp::kLe, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2, 4));

  res = chain->Search(FilterOp::kGt, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 3));

  res = chain->Search(FilterOp::kGe, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 3, 4));
}

TEST(NumericStorage, IndexSearch) {
  std::vector<int32_t> data_vec{-5, 5, -4, 4, -3, 3, 0};
  NumericStorage<int32_t> storage(&data_vec, ColumnType::kInt32, false);
  auto chain = storage.MakeChain();

  // -5, -3, -3, 3, 5, 0
  Indices common_indices = Indices::CreateWithIndexPayloadForTesting(
      {0, 4, 4, 5, 1, 6}, Indices::State::kNonmonotonic);
  SqlValue val = SqlValue::Long(3);

  auto indices = common_indices;
  chain->IndexSearch(FilterOp::kEq, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(3));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kNe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices),
              ElementsAre(0, 1, 2, 4, 5));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kLt, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices),
              ElementsAre(0, 1, 2, 5));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kLe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices),
              ElementsAre(0, 1, 2, 3, 5));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kGt, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(4));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kGe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(3, 4));
}

TEST(NumericStorage, IndexSearchCompareWithNegative) {
  std::vector<int32_t> data_vec{-5, 5, -4, 4, -3, 3, 0};
  NumericStorage<int32_t> storage(&data_vec, ColumnType::kInt32, false);
  auto chain = storage.MakeChain();

  // -5, -3, -3, 3, 5, 0
  Indices common_indices = Indices::CreateWithIndexPayloadForTesting(
      {0, 4, 4, 5, 1, 6}, Indices::State::kNonmonotonic);
  SqlValue val = SqlValue::Long(-3);

  auto indices = common_indices;
  chain->IndexSearch(FilterOp::kEq, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(1, 2));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kNe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices),
              ElementsAre(0, 3, 4, 5));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kLt, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(0));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kLe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(0, 1, 2));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kGt, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(3, 4, 5));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kGe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices),
              ElementsAre(1, 2, 3, 4, 5));
}

TEST(NumericStorage, SearchFast) {
  std::vector<uint32_t> data_vec(128);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage<uint32_t> storage(&data_vec, ColumnType::kUint32, false);
  auto chain = storage.MakeChain();
  RangeOrBitVector range_or_bv =
      chain->Search(FilterOp::kGe, SqlValue::Long(100), Range(0, 128));
  BitVector bv = std::move(range_or_bv).TakeIfBitVector();
  ASSERT_TRUE(range_or_bv.IsBitVector());
  ASSERT_EQ(bv.CountSetBits(), 28u);
  ASSERT_EQ(bv.IndexOfNthSet(0), 100u);
}

TEST(NumericStorage, SearchSorted) {
  std::vector<uint32_t> data_vec(128);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage<uint32_t> storage(&data_vec, ColumnType::kUint32, true);
  auto chain = storage.MakeChain();
  Range range = chain->Search(FilterOp::kGe, SqlValue::Long(100), Range(0, 128))
                    .TakeIfRange();
  ASSERT_EQ(range.size(), 28u);
  ASSERT_EQ(range.start, 100u);
  ASSERT_EQ(range.end, 128u);
}

TEST(NumericStorage, SearchSortedNe) {
  std::vector<uint32_t> data_vec(128);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage<uint32_t> storage(&data_vec, ColumnType::kUint32, true);
  auto chain = storage.MakeChain();
  BitVector bv =
      chain->Search(FilterOp::kNe, SqlValue::Long(100), Range(0, 128))
          .TakeIfBitVector();
  ASSERT_EQ(bv.CountSetBits(), 127u);
}

TEST(NumericStorage, SearchSortedSubset) {
  std::vector<uint32_t> data_vec(128);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage<uint32_t> storage(&data_vec, ColumnType::kUint32, true);
  auto chain = storage.MakeChain();
  Range range =
      chain->Search(FilterOp::kGe, SqlValue::Long(100), Range(102, 104))
          .TakeIfRange();
  ASSERT_EQ(range.size(), 2u);
  ASSERT_EQ(range.start, 102u);
  ASSERT_EQ(range.end, 104u);
}

TEST(NumericStorage, OrderedIndexSearch) {
  std::vector<uint32_t> data_vec{30, 40, 50, 60, 90, 80, 70, 0, 10, 20};
  std::vector<uint32_t> sorted_order_vec{7, 8, 9, 0, 1, 2, 3, 6, 5, 4};
  OrderedIndices sorted_order{sorted_order_vec.data(), 10,
                              Indices::State::kNonmonotonic};

  NumericStorage<uint32_t> storage(&data_vec, ColumnType::kUint32, false);
  auto chain = storage.MakeChain();

  Range range = chain->OrderedIndexSearch(FilterOp::kEq, SqlValue::Long(60),
                                          sorted_order);
  ASSERT_EQ(range.size(), 1u);
  ASSERT_EQ(range.start, 6u);
  ASSERT_EQ(range.end, 7u);

  range = chain->OrderedIndexSearch(FilterOp::kGt, SqlValue::Long(60),
                                    sorted_order);
  ASSERT_EQ(range.size(), 3u);
  ASSERT_EQ(range.start, 7u);
  ASSERT_EQ(range.end, 10u);

  range = chain->OrderedIndexSearch(FilterOp::kGe, SqlValue::Long(60),
                                    sorted_order);
  ASSERT_EQ(range.size(), 4u);
  ASSERT_EQ(range.start, 6u);
  ASSERT_EQ(range.end, 10u);

  range = chain->OrderedIndexSearch(FilterOp::kLt, SqlValue::Long(60),
                                    sorted_order);
  ASSERT_EQ(range.size(), 6u);
  ASSERT_EQ(range.start, 0u);
  ASSERT_EQ(range.end, 6u);

  range = chain->OrderedIndexSearch(FilterOp::kLe, SqlValue::Long(60),
                                    sorted_order);
  ASSERT_EQ(range.size(), 7u);
  ASSERT_EQ(range.start, 0u);
  ASSERT_EQ(range.end, 7u);
}

TEST(NumericStorage, SearchWithIntAsDouble) {
  std::vector<int32_t> data_vec{-5, 5, -4, 4, -3, 3, 0};
  NumericStorage<int32_t> storage(&data_vec, ColumnType::kInt32, false);
  auto chain = storage.MakeChain();
  Range test_range(1, 5);
  SqlValue val = SqlValue::Double(4);

  auto res = chain->Search(FilterOp::kEq, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3));

  res = chain->Search(FilterOp::kNe, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 2, 4));

  res = chain->Search(FilterOp::kLt, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2, 4));

  res = chain->Search(FilterOp::kLe, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2, 3, 4));

  res = chain->Search(FilterOp::kGt, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1));

  res = chain->Search(FilterOp::kGe, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 3));
}

TEST(NumericStorage, IndexSearchWithIntAsDouble) {
  std::vector<int32_t> data_vec{-5, 5, -4, 4, -3, 3, 0};
  NumericStorage<int32_t> storage(&data_vec, ColumnType::kInt32, false);
  auto chain = storage.MakeChain();

  // -5, -3, -3, 3, 5, 0
  Indices common_indices = Indices::CreateWithIndexPayloadForTesting(
      {0, 4, 4, 5, 1, 6}, Indices::State::kNonmonotonic);
  SqlValue val = SqlValue::Double(3);

  auto indices = common_indices;
  chain->IndexSearch(FilterOp::kEq, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(3));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kNe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices),
              ElementsAre(0, 1, 2, 4, 5));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kLt, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices),
              ElementsAre(0, 1, 2, 5));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kLe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices),
              ElementsAre(0, 1, 2, 3, 5));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kGt, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(4));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kGe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(3, 4));
}

TEST(NumericStorage, SearchInt32WithDouble) {
  std::vector<int32_t> data_vec{-5, 5, -4, 4, -3, 3, 0};
  NumericStorage<int32_t> storage(&data_vec, ColumnType::kInt32, false);
  auto chain = storage.MakeChain();
  Range test_range(1, 5);
  SqlValue val = SqlValue::Double(3.5);

  auto res = chain->Search(FilterOp::kEq, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), IsEmpty());

  res = chain->Search(FilterOp::kNe, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 2, 3, 4));

  res = chain->Search(FilterOp::kLt, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2, 4));

  res = chain->Search(FilterOp::kLe, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2, 4));

  res = chain->Search(FilterOp::kGt, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 3));

  res = chain->Search(FilterOp::kGe, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 3));
}

TEST(NumericStorage, SearchInt32WithNegDouble) {
  std::vector<int32_t> data_vec{-5, 5, -4, 4, -3, 3, 0};
  NumericStorage<int32_t> storage(&data_vec, ColumnType::kInt32, false);
  auto chain = storage.MakeChain();
  Range test_range(1, 5);
  SqlValue val = SqlValue::Double(-3.5);

  auto res = chain->Search(FilterOp::kEq, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), IsEmpty());

  res = chain->Search(FilterOp::kNe, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 2, 3, 4));

  res = chain->Search(FilterOp::kLt, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2));

  res = chain->Search(FilterOp::kLe, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2));

  res = chain->Search(FilterOp::kGt, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 3, 4));

  res = chain->Search(FilterOp::kGe, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 3, 4));
}

TEST(NumericStorage, IndexSearchInt32WithDouble) {
  std::vector<int32_t> data_vec{-5, 5, -4, 4, -3, 3, 0};
  NumericStorage<int32_t> storage(&data_vec, ColumnType::kInt32, false);
  auto chain = storage.MakeChain();

  // -5, -3, -3, 3, 5, 0
  Indices common_indices = Indices::CreateWithIndexPayloadForTesting(
      {0, 4, 4, 5, 1, 6}, Indices::State::kNonmonotonic);
  SqlValue val = SqlValue::Double(1.5);

  auto indices = common_indices;
  chain->IndexSearch(FilterOp::kEq, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), IsEmpty());

  indices = common_indices;
  chain->IndexSearch(FilterOp::kNe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices),
              ElementsAre(0, 1, 2, 3, 4, 5));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kLt, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices),
              ElementsAre(0, 1, 2, 5));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kLe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices),
              ElementsAre(0, 1, 2, 5));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kGt, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(3, 4));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kGe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(3, 4));
}

TEST(NumericStorage, IndexSearchInt32WithNegDouble) {
  std::vector<int32_t> data_vec{-5, 5, -4, 4, -3, 3, 0};
  NumericStorage<int32_t> storage(&data_vec, ColumnType::kInt32, false);
  auto chain = storage.MakeChain();

  // -5, -3, -3, 3, 5, 0
  Indices common_indices = Indices::CreateWithIndexPayloadForTesting(
      {0, 4, 4, 5, 1, 6}, Indices::State::kNonmonotonic);
  SqlValue val = SqlValue::Double(-2.5);

  auto indices = common_indices;
  chain->IndexSearch(FilterOp::kEq, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), IsEmpty());

  indices = common_indices;
  chain->IndexSearch(FilterOp::kNe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices),
              ElementsAre(0, 1, 2, 3, 4, 5));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kLt, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(0, 1, 2));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kLe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(0, 1, 2));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kGt, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(3, 4, 5));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kGe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(3, 4, 5));
}

TEST(NumericStorage, SearchUint32WithNegDouble) {
  std::vector<uint32_t> data_vec{0, 1, 2, 3, 4, 5};
  NumericStorage<uint32_t> storage(&data_vec, ColumnType::kInt32, false);
  auto chain = storage.MakeChain();
  Range test_range(1, 5);
  SqlValue val = SqlValue::Double(-3.5);

  auto res = chain->Search(FilterOp::kEq, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), IsEmpty());

  res = chain->Search(FilterOp::kNe, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 2, 3, 4));

  res = chain->Search(FilterOp::kLt, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), IsEmpty());

  res = chain->Search(FilterOp::kLe, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), IsEmpty());

  res = chain->Search(FilterOp::kGt, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 2, 3, 4));

  res = chain->Search(FilterOp::kGe, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 2, 3, 4));
}

TEST(NumericStorage, IndexSearchUint32WithNegDouble) {
  std::vector<uint32_t> data_vec{0, 1, 2, 3, 4, 5, 6};
  NumericStorage<uint32_t> storage(&data_vec, ColumnType::kInt32, false);
  auto chain = storage.MakeChain();

  Indices common_indices = Indices::CreateWithIndexPayloadForTesting(
      {0, 4, 4, 5, 1, 6}, Indices::State::kNonmonotonic);
  SqlValue val = SqlValue::Double(-2.5);

  auto indices = common_indices;
  chain->IndexSearch(FilterOp::kEq, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), IsEmpty());

  indices = common_indices;
  chain->IndexSearch(FilterOp::kNe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices),
              ElementsAre(0, 1, 2, 3, 4, 5));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kLt, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), IsEmpty());

  indices = common_indices;
  chain->IndexSearch(FilterOp::kLe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), IsEmpty());

  indices = common_indices;
  chain->IndexSearch(FilterOp::kGt, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices),
              ElementsAre(0, 1, 2, 3, 4, 5));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kGe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices),
              ElementsAre(0, 1, 2, 3, 4, 5));
}

TEST(NumericStorage, DoubleColumnWithIntThatCantBeRepresentedAsDouble) {
  // Sanity check that this value can't be represented as double.
  int64_t not_rep_i = 9007199254740993;
  EXPECT_FALSE(std::nextafter(static_cast<double>(not_rep_i), 1.0) ==
               static_cast<double>(not_rep_i));
  SqlValue val = SqlValue::Long(not_rep_i);

  std::vector<double> data_vec{9007199254740992.0, 9007199254740994.0};

  // Whether LongToDouble has the expected results.
  ASSERT_TRUE(compare::LongToDouble(not_rep_i, data_vec[0]) > 0);
  ASSERT_TRUE(compare::LongToDouble(not_rep_i, data_vec[1]) < 0);

  NumericStorage<double> storage(&data_vec, ColumnType::kDouble, false);
  auto chain = storage.MakeChain();
  Range test_range(0, 2);

  auto res = chain->Search(FilterOp::kEq, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), IsEmpty());

  res = chain->Search(FilterOp::kNe, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1));

  res = chain->Search(FilterOp::kLt, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0));

  res = chain->Search(FilterOp::kLe, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0));

  res = chain->Search(FilterOp::kGt, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1));

  res = chain->Search(FilterOp::kGe, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1));
}

TEST(NumericStorage, DoubleColumnWithNegIntThatCantBeRepresentedAsDouble) {
  // Sanity check that this value can't be represented as double.
  int64_t not_rep_i = -9007199254740993;
  EXPECT_FALSE(std::nextafter(static_cast<double>(not_rep_i), 1.0) ==
               static_cast<double>(not_rep_i));
  SqlValue val = SqlValue::Long(not_rep_i);

  std::vector<double> data_vec{-9007199254740992.0, -9007199254740994.0};

  // Whether LongToDouble has the expected results.
  ASSERT_TRUE(compare::LongToDouble(not_rep_i, data_vec[0]) < 0);
  ASSERT_TRUE(compare::LongToDouble(not_rep_i, data_vec[1]) > 0);

  NumericStorage<double> storage(&data_vec, ColumnType::kDouble, false);
  auto chain = storage.MakeChain();
  Range test_range(0, 2);

  auto res = chain->Search(FilterOp::kEq, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), IsEmpty());

  res = chain->Search(FilterOp::kNe, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1));

  res = chain->Search(FilterOp::kLt, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1));

  res = chain->Search(FilterOp::kLe, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1));

  res = chain->Search(FilterOp::kGt, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0));

  res = chain->Search(FilterOp::kGe, val, test_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0));
}

TEST(NumericStorage, StableSort) {
  std::vector<int64_t> data{
      -1, -100, 2, 100, 2,
  };
  NumericStorage<int64_t> storage(&data, ColumnType::kInt64, false);
  auto chain = storage.MakeChain();
  auto make_tokens = []() {
    return std::vector{
        Token{0, 0}, Token{1, 1}, Token{2, 2}, Token{3, 3}, Token{4, 4},
    };
  };
  {
    auto tokens = make_tokens();
    chain->StableSort(tokens.data(), tokens.data() + tokens.size(),
                      SortDirection::kAscending);
    ASSERT_THAT(utils::ExtractPayloadForTesting(tokens),
                ElementsAre(1, 0, 2, 4, 3));
  }
  {
    auto tokens = make_tokens();
    chain->StableSort(tokens.data(), tokens.data() + tokens.size(),
                      SortDirection::kDescending);
    ASSERT_THAT(utils::ExtractPayloadForTesting(tokens),
                ElementsAre(3, 2, 4, 0, 1));
  }
}

TEST(NumericStorage, DistinctFromIndexVector) {
  std::vector<int64_t> data{
      1, 100, 2, 100, 2,
  };
  NumericStorage<int64_t> storage(&data, ColumnType::kInt64, false);
  auto chain = storage.MakeChain();

  auto indices = Indices::CreateWithIndexPayloadForTesting(
      {2, 1, 0, 3}, Indices::State::kNonmonotonic);
  chain->Distinct(indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(0, 1, 2));
}

}  // namespace
}  // namespace column
}  // namespace perfetto::trace_processor
