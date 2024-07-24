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
#include "src/trace_processor/db/column/id_storage.h"

#include <cstdint>
#include <limits>
#include <vector>

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/db/column/data_layer.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/db/column/utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {

inline bool operator==(const Range& a, const Range& b) {
  return std::tie(a.start, a.end) == std::tie(b.start, b.end);
}

inline bool operator==(const BitVector& a, const BitVector& b) {
  return a.size() == b.size() && a.CountSetBits() == b.CountSetBits();
}

namespace column {
namespace {

using testing::ElementsAre;
using testing::IsEmpty;

using Indices = DataLayerChain::Indices;
using OrderedIndices = DataLayerChain::OrderedIndices;

TEST(IdStorage, InvalidSearchConstraints) {
  IdStorage storage;
  auto chain = storage.MakeChain();

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

  // With double
  ASSERT_EQ(
      chain->ValidateSearchConstraints(FilterOp::kGe, SqlValue::Double(-1)),
      SearchValidationResult::kAllData);

  // Value bounds
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

  SqlValue min_val = SqlValue::Long(-1);
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

TEST(IdStorage, SinglSearch) {
  IdStorage storage;
  auto chain = storage.MakeChain();

  ASSERT_EQ(chain->SingleSearch(FilterOp::kEq, SqlValue::Long(5), 5),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kEq, SqlValue::Long(5), 3),
            SingleSearchResult::kNoMatch);

  ASSERT_EQ(chain->SingleSearch(FilterOp::kNe, SqlValue::Long(5), 3),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kNe, SqlValue::Long(5), 5),
            SingleSearchResult::kNoMatch);

  ASSERT_EQ(chain->SingleSearch(FilterOp::kLe, SqlValue::Long(5), 5),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kLe, SqlValue::Long(5), 6),
            SingleSearchResult::kNoMatch);

  ASSERT_EQ(chain->SingleSearch(FilterOp::kLt, SqlValue::Long(5), 4),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kLt, SqlValue::Long(5), 6),
            SingleSearchResult::kNoMatch);

  ASSERT_EQ(chain->SingleSearch(FilterOp::kGe, SqlValue::Long(5), 5),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kGe, SqlValue::Long(5), 4),
            SingleSearchResult::kNoMatch);

  ASSERT_EQ(chain->SingleSearch(FilterOp::kGt, SqlValue::Long(5), 6),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kGt, SqlValue::Long(5), 4),
            SingleSearchResult::kNoMatch);

  ASSERT_EQ(chain->SingleSearch(FilterOp::kEq, SqlValue::Double(5), 4),
            SingleSearchResult::kNeedsFullSearch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kEq, SqlValue::String(""), 4),
            SingleSearchResult::kNeedsFullSearch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kGlob, SqlValue::Long(5), 4),
            SingleSearchResult::kNoMatch);
}

TEST(IdStorage, SearchEqSimple) {
  IdStorage storage;
  auto chain = storage.MakeChain();
  Range range = chain->Search(FilterOp::kEq, SqlValue::Long(15), Range(10, 20))
                    .TakeIfRange();
  ASSERT_EQ(range.size(), 1u);
  ASSERT_EQ(range.start, 15u);
  ASSERT_EQ(range.end, 16u);
}

TEST(IdStorage, SearchEqOnRangeBoundary) {
  IdStorage storage;
  auto chain = storage.MakeChain();
  Range range = chain->Search(FilterOp::kEq, SqlValue::Long(20), Range(10, 20))
                    .TakeIfRange();
  ASSERT_EQ(range.size(), 0u);
}

TEST(IdStorage, SearchEqOutsideRange) {
  IdStorage storage;
  auto chain = storage.MakeChain();
  Range range = chain->Search(FilterOp::kEq, SqlValue::Long(25), Range(10, 20))
                    .TakeIfRange();
  ASSERT_EQ(range.size(), 0u);
}

TEST(IdStorage, SearchEqTooBig) {
  IdStorage storage;
  auto chain = storage.MakeChain();
  Range range = chain->Search(FilterOp::kEq, SqlValue::Long(125), Range(10, 20))
                    .TakeIfRange();
  ASSERT_EQ(range.size(), 0u);
}

TEST(IdStorage, SearchSimple) {
  IdStorage storage;
  auto chain = storage.MakeChain();
  SqlValue val = SqlValue::Long(5);
  Range filter_range(3, 7);

  FilterOp op = FilterOp::kEq;
  auto res = chain->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(5));

  op = FilterOp::kNe;
  res = chain->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3, 4, 6));

  op = FilterOp::kLe;
  res = chain->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3, 4, 5));

  op = FilterOp::kLt;
  res = chain->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3, 4));

  op = FilterOp::kGe;
  res = chain->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(5, 6));

  op = FilterOp::kGt;
  res = chain->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(6));
}

TEST(IdStorage, IndexSearchSimple) {
  IdStorage storage;
  auto chain = storage.MakeChain();
  SqlValue val = SqlValue::Long(5);

  auto common_indices = Indices::CreateWithIndexPayloadForTesting(
      {5, 4, 3, 9, 8, 7}, Indices::State::kNonmonotonic);

  auto indices = common_indices;
  chain->IndexSearch(FilterOp::kEq, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(0));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kNe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices),
              ElementsAre(1, 2, 3, 4, 5));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kLe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(0, 1, 2));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kLt, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(1, 2));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kGe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices),
              ElementsAre(0, 3, 4, 5));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kGt, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(3, 4, 5));
}

TEST(IdStorage, OrderedIndexSearch) {
  IdStorage storage;
  auto chain = storage.MakeChain();

  std::vector<uint32_t> indices_vec{0, 1, 2, 4, 4};
  OrderedIndices indices{indices_vec.data(), 5, Indices::State::kMonotonic};

  Range range =
      chain->OrderedIndexSearch(FilterOp::kEq, SqlValue::Long(2), indices);
  ASSERT_EQ(range.start, 2u);
  ASSERT_EQ(range.end, 3u);

  range = chain->OrderedIndexSearch(FilterOp::kGt, SqlValue::Long(2), indices);
  ASSERT_EQ(range.start, 3u);
  ASSERT_EQ(range.end, 5u);

  range = chain->OrderedIndexSearch(FilterOp::kGe, SqlValue::Long(2), indices);
  ASSERT_EQ(range.start, 2u);
  ASSERT_EQ(range.end, 5u);

  range = chain->OrderedIndexSearch(FilterOp::kLt, SqlValue::Long(2), indices);
  ASSERT_EQ(range.start, 0u);
  ASSERT_EQ(range.end, 2u);

  range = chain->OrderedIndexSearch(FilterOp::kLe, SqlValue::Long(2), indices);
  ASSERT_EQ(range.start, 0u);
  ASSERT_EQ(range.end, 3u);
}

TEST(IdStorage, IndexSearchEqTooBig) {
  IdStorage storage;
  auto chain = storage.MakeChain();

  auto indices = Indices::CreateWithIndexPayloadForTesting(
      {1, 3, 5, 7, 9, 11, 2, 4}, Indices::State::kNonmonotonic);
  chain->IndexSearch(FilterOp::kEq, SqlValue::Long(20), indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), IsEmpty());
}

TEST(IdStorage, SearchWithIdAsDoubleSimple) {
  IdStorage storage;
  auto chain = storage.MakeChain();
  SqlValue double_val = SqlValue::Double(15.0);
  SqlValue long_val = SqlValue::Long(15);
  Range range(10, 20);

  auto res_double = chain->Search(FilterOp::kEq, double_val, range);
  auto res_long = chain->Search(FilterOp::kEq, long_val, range);
  ASSERT_EQ(utils::ToIndexVectorForTests(res_double),
            utils::ToIndexVectorForTests(res_long));

  res_double = chain->Search(FilterOp::kNe, double_val, range);
  res_long = chain->Search(FilterOp::kNe, long_val, range);
  ASSERT_EQ(utils::ToIndexVectorForTests(res_double),
            utils::ToIndexVectorForTests(res_long));

  res_double = chain->Search(FilterOp::kLe, double_val, range);
  res_long = chain->Search(FilterOp::kLe, long_val, range);
  ASSERT_EQ(utils::ToIndexVectorForTests(res_double),
            utils::ToIndexVectorForTests(res_long));

  res_double = chain->Search(FilterOp::kLt, double_val, range);
  res_long = chain->Search(FilterOp::kLt, long_val, range);
  ASSERT_EQ(utils::ToIndexVectorForTests(res_double),
            utils::ToIndexVectorForTests(res_long));

  res_double = chain->Search(FilterOp::kGe, double_val, range);
  res_long = chain->Search(FilterOp::kGe, long_val, range);
  ASSERT_EQ(utils::ToIndexVectorForTests(res_double),
            utils::ToIndexVectorForTests(res_long));

  res_double = chain->Search(FilterOp::kGt, double_val, range);
  res_long = chain->Search(FilterOp::kGt, long_val, range);
  ASSERT_EQ(utils::ToIndexVectorForTests(res_double),
            utils::ToIndexVectorForTests(res_long));
}

TEST(IdStorage, SearchWithIdAsDouble) {
  IdStorage storage;
  auto chain = storage.MakeChain();
  Range range(10, 20);
  SqlValue val = SqlValue::Double(15.5);

  auto res = chain->Search(FilterOp::kEq, val, range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), IsEmpty());

  res = chain->Search(FilterOp::kNe, val, range);
  ASSERT_EQ(utils::ToIndexVectorForTests(res).size(), 20u);

  res = chain->Search(FilterOp::kLe, val, range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res),
              ElementsAre(10, 11, 12, 13, 14, 15));

  res = chain->Search(FilterOp::kLt, val, range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res),
              ElementsAre(10, 11, 12, 13, 14, 15));

  res = chain->Search(FilterOp::kGe, val, range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(16, 17, 18, 19));

  res = chain->Search(FilterOp::kGt, val, range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(16, 17, 18, 19));
}

TEST(IdStorage, StableSort) {
  IdStorage storage;
  auto chain = storage.MakeChain();
  std::vector tokens{
      Token{0, 0}, Token{1, 1}, Token{2, 2}, Token{3, 3}, Token{4, 4},
  };
  chain->StableSort(tokens.data(), tokens.data() + tokens.size(),
                    SortDirection::kAscending);
  ASSERT_THAT(utils::ExtractPayloadForTesting(tokens),
              ElementsAre(0, 1, 2, 3, 4));

  chain->StableSort(tokens.data(), tokens.data() + tokens.size(),
                    SortDirection::kDescending);
  ASSERT_THAT(utils::ExtractPayloadForTesting(tokens),
              ElementsAre(4, 3, 2, 1, 0));
}

TEST(IdStorage, Distinct) {
  IdStorage storage;
  auto chain = storage.MakeChain();

  auto indices = Indices::CreateWithIndexPayloadForTesting(
      {1, 1, 0, 3}, Indices::State::kNonmonotonic);
  chain->Distinct(indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(0, 2, 3));
}

}  // namespace
}  // namespace column
}  // namespace perfetto::trace_processor
