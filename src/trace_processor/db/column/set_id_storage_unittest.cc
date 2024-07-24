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
#include "src/trace_processor/db/column/set_id_storage.h"

#include <cstdint>
#include <limits>
#include <tuple>
#include <vector>

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/db/column/data_layer.h"
#include "src/trace_processor/db/column/numeric_storage.h"
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

TEST(SetIdStorage, SearchSingle) {
  std::vector<uint32_t> storage_data{0, 0, 2, 2, 4, 4, 6, 6};
  SetIdStorage storage(&storage_data);
  auto chain = storage.MakeChain();

  ASSERT_EQ(chain->SingleSearch(FilterOp::kEq, SqlValue::Long(4), 4),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kEq, SqlValue::Long(4), 3),
            SingleSearchResult::kNoMatch);

  ASSERT_EQ(chain->SingleSearch(FilterOp::kNe, SqlValue::Long(4), 3),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kNe, SqlValue::Long(4), 4),
            SingleSearchResult::kNoMatch);

  ASSERT_EQ(chain->SingleSearch(FilterOp::kGe, SqlValue::Long(4), 4),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kGe, SqlValue::Long(4), 1),
            SingleSearchResult::kNoMatch);

  ASSERT_EQ(chain->SingleSearch(FilterOp::kGt, SqlValue::Long(4), 6),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kGt, SqlValue::Long(4), 4),
            SingleSearchResult::kNoMatch);

  ASSERT_EQ(chain->SingleSearch(FilterOp::kLe, SqlValue::Long(4), 4),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kLe, SqlValue::Long(4), 6),
            SingleSearchResult::kNoMatch);

  ASSERT_EQ(chain->SingleSearch(FilterOp::kLt, SqlValue::Long(4), 3),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kLt, SqlValue::Long(4), 4),
            SingleSearchResult::kNoMatch);
}

TEST(SetIdStorage, InvalidSearchConstraints) {
  std::vector<uint32_t> storage_data{0, 0, 0, 3, 3, 3, 6, 6, 6, 9, 9, 9};
  SetIdStorage storage(&storage_data);
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

TEST(SetIdStorage, SearchSimple) {
  std::vector<uint32_t> storage_data{0, 0, 2, 2, 4, 4, 6, 6};
  SetIdStorage storage(&storage_data);
  auto chain = storage.MakeChain();
  SqlValue val = SqlValue::Long(4);
  Range filter_range(1, 7);

  FilterOp op = FilterOp::kEq;
  auto res = chain->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(4, 5));

  op = FilterOp::kNe;
  res = chain->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 2, 3, 6));

  op = FilterOp::kLe;
  res = chain->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 2, 3, 4, 5));

  op = FilterOp::kLt;
  res = chain->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 2, 3));

  op = FilterOp::kGe;
  res = chain->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(4, 5, 6));

  op = FilterOp::kGt;
  res = chain->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(6));
}

TEST(SetIdStorage, IndexSearchSimple) {
  std::vector<uint32_t> storage_data{0, 0, 2, 2, 4, 4, 6, 6};
  SetIdStorage storage(&storage_data);
  auto chain = storage.MakeChain();
  SqlValue val = SqlValue::Long(4);
  // 6, 4, 2, 0
  auto common_indices = Indices::CreateWithIndexPayloadForTesting(
      {6, 4, 2, 0}, Indices::State::kNonmonotonic);

  auto indices = common_indices;
  chain->IndexSearch(FilterOp::kEq, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(1));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kNe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(0, 2, 3));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kLe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(1, 2, 3));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kLt, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(2, 3));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kGe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(0, 1));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kGt, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(0));
}

TEST(SetIdStorage, OrderedIndexSearchSimple) {
  std::vector<uint32_t> storage_data{0, 0, 2, 2, 4, 4, 6, 6};
  SetIdStorage storage(&storage_data);
  auto chain = storage.MakeChain();

  // 0, 2, 2, 4
  std::vector<uint32_t> indices_vec{0, 3, 3, 5};
  OrderedIndices indices{indices_vec.data(), 4, Indices::State::kMonotonic};

  Range range =
      chain->OrderedIndexSearch(FilterOp::kEq, SqlValue::Long(2), indices);
  ASSERT_EQ(range.start, 1u);
  ASSERT_EQ(range.end, 3u);

  range = chain->OrderedIndexSearch(FilterOp::kGt, SqlValue::Long(2), indices);
  ASSERT_EQ(range.start, 3u);
  ASSERT_EQ(range.end, 4u);

  range = chain->OrderedIndexSearch(FilterOp::kGe, SqlValue::Long(2), indices);
  ASSERT_EQ(range.start, 1u);
  ASSERT_EQ(range.end, 4u);

  range = chain->OrderedIndexSearch(FilterOp::kLt, SqlValue::Long(2), indices);
  ASSERT_EQ(range.start, 0u);
  ASSERT_EQ(range.end, 1u);

  range = chain->OrderedIndexSearch(FilterOp::kLe, SqlValue::Long(2), indices);
  ASSERT_EQ(range.start, 0u);
  ASSERT_EQ(range.end, 3u);
}

TEST(SetIdStorage, SearchEqSimple) {
  std::vector<uint32_t> storage_data{0, 0, 0, 3, 3, 3, 6, 6, 6, 9, 9, 9};

  SetIdStorage storage(&storage_data);
  auto chain = storage.MakeChain();
  Range range = chain->Search(FilterOp::kEq, SqlValue::Long(3), Range(4, 10))
                    .TakeIfRange();

  ASSERT_EQ(range.size(), 2u);
  ASSERT_EQ(range.start, 4u);
  ASSERT_EQ(range.end, 6u);
}

TEST(SetIdStorageUnittest, SearchEqFalse) {
  std::vector<uint32_t> storage_data{0, 0, 0, 3, 3, 3, 6, 6, 6, 9, 9, 9};
  SetIdStorage storage(&storage_data);
  auto chain = storage.MakeChain();

  Range range = chain->Search(FilterOp::kEq, SqlValue::Long(5), Range(4, 10))
                    .TakeIfRange();

  ASSERT_TRUE(range.empty());
}

TEST(SetIdStorage, SearchEqOnRangeBoundary) {
  std::vector<uint32_t> storage_data{0, 0, 0, 3, 3, 3, 6, 6, 6, 9, 9, 9};

  SetIdStorage storage(&storage_data);
  auto chain = storage.MakeChain();
  Range range = chain->Search(FilterOp::kEq, SqlValue::Long(9), Range(6, 9))
                    .TakeIfRange();
  ASSERT_EQ(range.size(), 0u);
}

TEST(SetIdStorage, SearchEqOutsideRange) {
  std::vector<uint32_t> storage_data{0, 0, 0, 3, 3, 3, 6, 6, 6, 9, 9, 9};

  SetIdStorage storage(&storage_data);
  auto chain = storage.MakeChain();
  Range range = chain->Search(FilterOp::kEq, SqlValue::Long(12), Range(6, 9))
                    .TakeIfRange();
  ASSERT_EQ(range.size(), 0u);
}

TEST(SetIdStorage, SearchEqTooBig) {
  std::vector<uint32_t> storage_data{0, 0, 0, 3, 3, 3, 6, 6, 6, 9, 9, 9};

  SetIdStorage storage(&storage_data);
  auto chain = storage.MakeChain();
  Range range = chain->Search(FilterOp::kEq, SqlValue::Long(100), Range(6, 9))
                    .TakeIfRange();
  ASSERT_EQ(range.size(), 0u);
}

TEST(SetIdStorage, IndexSearchEqTooBig) {
  std::vector<uint32_t> storage_data{0, 0, 0, 3, 3, 3, 6, 6, 6, 9, 9, 9};
  SetIdStorage storage(&storage_data);
  auto chain = storage.MakeChain();

  // {0, 3, 3, 6, 9, 9, 0, 3}
  auto indices = Indices::CreateWithIndexPayloadForTesting(
      {1, 3, 5, 7, 9, 11, 2, 4}, Indices::State::kNonmonotonic);
  chain->IndexSearch(FilterOp::kEq, SqlValue::Long(10), indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), IsEmpty());
}

TEST(SetIdStorageUnittest, IndexSearchEqFalse) {
  std::vector<uint32_t> storage_data{0, 0, 0, 3, 3, 3, 6, 6, 6, 9, 9, 9};
  SetIdStorage storage(&storage_data);
  auto chain = storage.MakeChain();

  // {0, 3, 3, 6, 9, 9, 0, 3}
  auto indices = Indices::CreateWithIndexPayloadForTesting(
      {1, 3, 5, 7, 9, 11, 2, 4}, Indices::State::kNonmonotonic);
  chain->IndexSearch(FilterOp::kEq, SqlValue::Long(5), indices);

  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), IsEmpty());
}

TEST(SetIdStorage, SearchWithIdAsSimpleDoubleIsInt) {
  std::vector<uint32_t> storage_data{0, 0, 0, 3, 3, 3, 6, 6, 6, 9, 9, 9};
  SetIdStorage storage(&storage_data);
  auto chain = storage.MakeChain();
  SqlValue double_val = SqlValue::Double(7.0);
  SqlValue long_val = SqlValue::Long(7);
  Range range(1, 9);

  FilterOp op = FilterOp::kEq;
  auto double_res = chain->Search(op, double_val, range);
  auto int_res = chain->Search(op, long_val, range);
  ASSERT_EQ(utils::ToIndexVectorForTests(double_res),
            utils::ToIndexVectorForTests(int_res));

  op = FilterOp::kNe;
  double_res = chain->Search(op, double_val, range);
  int_res = chain->Search(op, long_val, range);
  ASSERT_EQ(utils::ToIndexVectorForTests(double_res),
            utils::ToIndexVectorForTests(int_res));

  op = FilterOp::kLe;
  double_res = chain->Search(op, double_val, range);
  int_res = chain->Search(op, long_val, range);
  ASSERT_EQ(utils::ToIndexVectorForTests(double_res),
            utils::ToIndexVectorForTests(int_res));

  op = FilterOp::kLt;
  double_res = chain->Search(op, double_val, range);
  int_res = chain->Search(op, long_val, range);
  ASSERT_EQ(utils::ToIndexVectorForTests(double_res),
            utils::ToIndexVectorForTests(int_res));

  op = FilterOp::kGe;
  double_res = chain->Search(op, double_val, range);
  int_res = chain->Search(op, long_val, range);
  ASSERT_EQ(utils::ToIndexVectorForTests(double_res),
            utils::ToIndexVectorForTests(int_res));

  op = FilterOp::kGt;
  double_res = chain->Search(op, double_val, range);
  int_res = chain->Search(op, long_val, range);
  ASSERT_EQ(utils::ToIndexVectorForTests(double_res),
            utils::ToIndexVectorForTests(int_res));
}

TEST(SetIdStorage, SearchWithIdAsDouble) {
  std::vector<uint32_t> storage_data{0, 0, 0, 3, 3, 3, 6, 6, 6, 9, 9, 9};
  SetIdStorage storage(&storage_data);
  auto chain = storage.MakeChain();
  SqlValue val = SqlValue::Double(7.5);
  Range range(5, 10);

  Range res = chain->Search(FilterOp::kEq, val, range).TakeIfRange();
  ASSERT_EQ(res, Range());

  res = chain->Search(FilterOp::kNe, val, range).TakeIfRange();
  ASSERT_EQ(res, Range(0, 10));

  res = chain->Search(FilterOp::kLe, val, range).TakeIfRange();
  ASSERT_EQ(res, Range(5, 9));

  res = chain->Search(FilterOp::kLt, val, range).TakeIfRange();
  ASSERT_EQ(res, Range(5, 9));

  res = chain->Search(FilterOp::kGe, val, range).TakeIfRange();
  ASSERT_EQ(res, Range(9, 10));

  res = chain->Search(FilterOp::kGt, val, range).TakeIfRange();
  ASSERT_EQ(res, Range(9, 10));
}

TEST(SetIdStorage, StableSort) {
  std::vector<uint32_t> storage_data{0, 0, 0, 3, 3};
  SetIdStorage storage(&storage_data);
  auto chain = storage.MakeChain();
  auto make_tokens = []() {
    return std::vector{
        Token{3, 3}, Token{2, 2}, Token{1, 1}, Token{0, 0}, Token{4, 4},
    };
  };
  {
    auto tokens = make_tokens();
    chain->StableSort(tokens.data(), tokens.data() + tokens.size(),
                      SortDirection::kAscending);
    ASSERT_THAT(utils::ExtractPayloadForTesting(tokens),
                ElementsAre(2, 1, 0, 3, 4));
  }
  {
    auto tokens = make_tokens();
    chain->StableSort(tokens.data(), tokens.data() + tokens.size(),
                      SortDirection::kDescending);
    ASSERT_THAT(utils::ExtractPayloadForTesting(tokens),
                ElementsAre(3, 4, 2, 1, 0));
  }
}

TEST(SetIdStorage, Distinct) {
  std::vector<uint32_t> storage_data{0, 0, 0, 3, 3, 3, 6, 6, 6, 9, 9, 9};
  SetIdStorage storage(&storage_data);
  auto chain = storage.MakeChain();

  auto indices = Indices::CreateWithIndexPayloadForTesting(
      {10, 9, 0, 1, 4}, Indices::State::kNonmonotonic);
  chain->Distinct(indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(0, 2, 4));
}

}  // namespace
}  // namespace column
}  // namespace perfetto::trace_processor
