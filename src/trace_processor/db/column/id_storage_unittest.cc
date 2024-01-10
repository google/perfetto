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
#include <limits>

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/db/column/column.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/db/column/utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {

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

TEST(IdStorage, InvalidSearchConstraints) {
  IdStorage storage(100);

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

  // With double
  ASSERT_EQ(
      storage.ValidateSearchConstraints(SqlValue::Double(-1), FilterOp::kGe),
      SearchValidationResult::kAllData);

  // Value bounds
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

  SqlValue min_val = SqlValue::Long(-1);
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

TEST(IdStorage, SearchEqSimple) {
  IdStorage storage(100);
  Range range = storage.Search(FilterOp::kEq, SqlValue::Long(15), Range(10, 20))
                    .TakeIfRange();
  ASSERT_EQ(range.size(), 1u);
  ASSERT_EQ(range.start, 15u);
  ASSERT_EQ(range.end, 16u);
}

TEST(IdStorage, SearchEqOnRangeBoundary) {
  IdStorage storage(100);
  Range range = storage.Search(FilterOp::kEq, SqlValue::Long(20), Range(10, 20))
                    .TakeIfRange();
  ASSERT_EQ(range.size(), 0u);
}

TEST(IdStorage, SearchEqOutsideRange) {
  IdStorage storage(100);
  Range range = storage.Search(FilterOp::kEq, SqlValue::Long(25), Range(10, 20))
                    .TakeIfRange();
  ASSERT_EQ(range.size(), 0u);
}

TEST(IdStorage, SearchEqTooBig) {
  IdStorage storage(100);
  Range range =
      storage.Search(FilterOp::kEq, SqlValue::Long(125), Range(10, 20))
          .TakeIfRange();
  ASSERT_EQ(range.size(), 0u);
}

TEST(IdStorage, SearchSimple) {
  IdStorage storage(10);
  SqlValue val = SqlValue::Long(5);
  Range filter_range(3, 7);

  FilterOp op = FilterOp::kEq;
  auto res = storage.Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(5));

  op = FilterOp::kNe;
  res = storage.Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3, 4, 6));

  op = FilterOp::kLe;
  res = storage.Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3, 4, 5));

  op = FilterOp::kLt;
  res = storage.Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3, 4));

  op = FilterOp::kGe;
  res = storage.Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(5, 6));

  op = FilterOp::kGt;
  res = storage.Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(6));
}

TEST(IdStorage, IndexSearchSimple) {
  IdStorage storage(10);
  SqlValue val = SqlValue::Long(5);
  std::vector<uint32_t> indices{5, 4, 3, 9, 8, 7};
  uint32_t indices_count = 6;

  FilterOp op = FilterOp::kEq;
  auto res = storage.IndexSearch(op, val, indices.data(), indices_count, false);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0));

  op = FilterOp::kNe;
  res = storage.IndexSearch(op, val, indices.data(), indices_count, false);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 2, 3, 4, 5));

  op = FilterOp::kLe;
  res = storage.IndexSearch(op, val, indices.data(), indices_count, false);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 2));

  op = FilterOp::kLt;
  res = storage.IndexSearch(op, val, indices.data(), indices_count, false);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 2));

  op = FilterOp::kGe;
  res = storage.IndexSearch(op, val, indices.data(), indices_count, false);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 3, 4, 5));

  op = FilterOp::kGt;
  res = storage.IndexSearch(op, val, indices.data(), indices_count, false);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3, 4, 5));
}

TEST(IdStorage, IndexSearchEqTooBig) {
  IdStorage storage(12);
  std::vector<uint32_t> indices{1, 3, 5, 7, 9, 11, 2, 4};

  BitVector bv =
      storage
          .IndexSearch(FilterOp::kEq, SqlValue::Long(20), indices.data(),
                       static_cast<uint32_t>(indices.size()), false)
          .TakeIfBitVector();

  ASSERT_EQ(bv.CountSetBits(), 0u);
}

TEST(IdStorage, SearchWithIdAsDoubleSimple) {
  IdStorage storage(100);
  SqlValue double_val = SqlValue::Double(15.0);
  SqlValue long_val = SqlValue::Long(15);
  Range range(10, 20);

  auto res_double = storage.Search(FilterOp::kEq, double_val, range);
  auto res_long = storage.Search(FilterOp::kEq, long_val, range);
  ASSERT_EQ(utils::ToIndexVectorForTests(res_double),
            utils::ToIndexVectorForTests(res_long));

  res_double = storage.Search(FilterOp::kNe, double_val, range);
  res_long = storage.Search(FilterOp::kNe, long_val, range);
  ASSERT_EQ(utils::ToIndexVectorForTests(res_double),
            utils::ToIndexVectorForTests(res_long));

  res_double = storage.Search(FilterOp::kLe, double_val, range);
  res_long = storage.Search(FilterOp::kLe, long_val, range);
  ASSERT_EQ(utils::ToIndexVectorForTests(res_double),
            utils::ToIndexVectorForTests(res_long));

  res_double = storage.Search(FilterOp::kLt, double_val, range);
  res_long = storage.Search(FilterOp::kLt, long_val, range);
  ASSERT_EQ(utils::ToIndexVectorForTests(res_double),
            utils::ToIndexVectorForTests(res_long));

  res_double = storage.Search(FilterOp::kGe, double_val, range);
  res_long = storage.Search(FilterOp::kGe, long_val, range);
  ASSERT_EQ(utils::ToIndexVectorForTests(res_double),
            utils::ToIndexVectorForTests(res_long));

  res_double = storage.Search(FilterOp::kGt, double_val, range);
  res_long = storage.Search(FilterOp::kGt, long_val, range);
  ASSERT_EQ(utils::ToIndexVectorForTests(res_double),
            utils::ToIndexVectorForTests(res_long));
}

TEST(IdStorage, SearchWithIdAsDouble) {
  IdStorage storage(100);
  Range range(10, 20);
  SqlValue val = SqlValue::Double(15.5);

  auto res = storage.Search(FilterOp::kEq, val, range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), IsEmpty());

  res = storage.Search(FilterOp::kNe, val, range);
  ASSERT_EQ(utils::ToIndexVectorForTests(res).size(), 20u);

  res = storage.Search(FilterOp::kLe, val, range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res),
              ElementsAre(10, 11, 12, 13, 14, 15));

  res = storage.Search(FilterOp::kLt, val, range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res),
              ElementsAre(10, 11, 12, 13, 14, 15));

  res = storage.Search(FilterOp::kGe, val, range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(16, 17, 18, 19));

  res = storage.Search(FilterOp::kGt, val, range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(16, 17, 18, 19));
}

TEST(IdStorage, Sort) {
  std::vector<uint32_t> order{4, 3, 6, 1, 5};
  IdStorage storage(10);
  storage.Sort(order.data(), 5);

  std::vector<uint32_t> sorted_order{1, 3, 4, 5, 6};
  ASSERT_EQ(order, sorted_order);
}

}  // namespace
}  // namespace column
}  // namespace trace_processor
}  // namespace perfetto
