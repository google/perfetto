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

TEST(SetIdStorage, InvalidSearchConstraints) {
  std::vector<uint32_t> storage_data{0, 0, 0, 3, 3, 3, 6, 6, 6, 9, 9, 9};
  SetIdStorage storage(&storage_data);
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

TEST(SetIdStorage, SearchSimple) {
  std::vector<uint32_t> storage_data{0, 0, 2, 2, 4, 4, 6, 6};
  SetIdStorage storage(&storage_data);
  SqlValue val = SqlValue::Long(4);
  Range filter_range(1, 7);

  FilterOp op = FilterOp::kEq;
  auto res = storage.Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(4, 5));

  op = FilterOp::kNe;
  res = storage.Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 2, 3, 6));

  op = FilterOp::kLe;
  res = storage.Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 2, 3, 4, 5));

  op = FilterOp::kLt;
  res = storage.Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 2, 3));

  op = FilterOp::kGe;
  res = storage.Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(4, 5, 6));

  op = FilterOp::kGt;
  res = storage.Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(6));
}

TEST(SetIdStorage, IndexSearchSimple) {
  std::vector<uint32_t> storage_data{0, 0, 3, 3, 6, 6, 9, 9};
  SetIdStorage storage(&storage_data);
  SqlValue val = SqlValue::Long(6);
  // 9, 6, 3, 0
  std::vector<uint32_t> indices{6, 4, 2, 0};
  uint32_t indices_count = 4;

  FilterOp op = FilterOp::kEq;
  auto res = storage.IndexSearch(op, val, indices.data(), indices_count, false);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1));

  op = FilterOp::kNe;
  res = storage.IndexSearch(op, val, indices.data(), indices_count, false);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 2, 3));

  op = FilterOp::kLe;
  res = storage.IndexSearch(op, val, indices.data(), indices_count, false);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 2, 3));

  op = FilterOp::kLt;
  res = storage.IndexSearch(op, val, indices.data(), indices_count, false);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2, 3));

  op = FilterOp::kGe;
  res = storage.IndexSearch(op, val, indices.data(), indices_count, false);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1));

  op = FilterOp::kGt;
  res = storage.IndexSearch(op, val, indices.data(), indices_count, false);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0));
}

TEST(SetIdStorage, SearchEqSimple) {
  std::vector<uint32_t> storage_data{0, 0, 0, 3, 3, 3, 6, 6, 6, 9, 9, 9};

  SetIdStorage storage(&storage_data);
  Range range = storage.Search(FilterOp::kEq, SqlValue::Long(3), Range(4, 10))
                    .TakeIfRange();

  ASSERT_EQ(range.size(), 2u);
  ASSERT_EQ(range.start, 4u);
  ASSERT_EQ(range.end, 6u);
}

TEST(SetIdStorage, SearchEqOnRangeBoundary) {
  std::vector<uint32_t> storage_data{0, 0, 0, 3, 3, 3, 6, 6, 6, 9, 9, 9};

  SetIdStorage storage(&storage_data);
  Range range = storage.Search(FilterOp::kEq, SqlValue::Long(9), Range(6, 9))
                    .TakeIfRange();
  ASSERT_EQ(range.size(), 0u);
}

TEST(SetIdStorage, SearchEqOutsideRange) {
  std::vector<uint32_t> storage_data{0, 0, 0, 3, 3, 3, 6, 6, 6, 9, 9, 9};

  SetIdStorage storage(&storage_data);
  Range range = storage.Search(FilterOp::kEq, SqlValue::Long(12), Range(6, 9))
                    .TakeIfRange();
  ASSERT_EQ(range.size(), 0u);
}

TEST(SetIdStorage, SearchEqTooBig) {
  std::vector<uint32_t> storage_data{0, 0, 0, 3, 3, 3, 6, 6, 6, 9, 9, 9};

  SetIdStorage storage(&storage_data);
  Range range = storage.Search(FilterOp::kEq, SqlValue::Long(100), Range(6, 9))
                    .TakeIfRange();
  ASSERT_EQ(range.size(), 0u);
}

TEST(SetIdStorage, IndexSearchEqTooBig) {
  std::vector<uint32_t> storage_data{0, 0, 0, 3, 3, 3, 6, 6, 6, 9, 9, 9};
  SetIdStorage storage(&storage_data);

  // {0, 3, 3, 6, 9, 9, 0, 3}
  std::vector<uint32_t> indices{1, 3, 5, 7, 9, 11, 2, 4};

  BitVector bv = storage
                     .IndexSearch(FilterOp::kEq, SqlValue::Long(10),
                                  indices.data(), 8, false)
                     .TakeIfBitVector();

  ASSERT_EQ(bv.CountSetBits(), 0u);
}

TEST(SetIdStorage, SearchWithIdAsSimpleDoubleIsInt) {
  std::vector<uint32_t> storage_data{0, 0, 0, 3, 3, 3, 6, 6, 6, 9, 9, 9};
  SetIdStorage storage(&storage_data);
  SqlValue double_val = SqlValue::Double(7.0);
  SqlValue long_val = SqlValue::Long(7);
  Range range(1, 9);

  FilterOp op = FilterOp::kEq;
  auto double_res = storage.Search(op, double_val, range);
  auto int_res = storage.Search(op, long_val, range);
  ASSERT_EQ(utils::ToIndexVectorForTests(double_res),
            utils::ToIndexVectorForTests(int_res));

  op = FilterOp::kNe;
  double_res = storage.Search(op, double_val, range);
  int_res = storage.Search(op, long_val, range);
  ASSERT_EQ(utils::ToIndexVectorForTests(double_res),
            utils::ToIndexVectorForTests(int_res));

  op = FilterOp::kLe;
  double_res = storage.Search(op, double_val, range);
  int_res = storage.Search(op, long_val, range);
  ASSERT_EQ(utils::ToIndexVectorForTests(double_res),
            utils::ToIndexVectorForTests(int_res));

  op = FilterOp::kLt;
  double_res = storage.Search(op, double_val, range);
  int_res = storage.Search(op, long_val, range);
  ASSERT_EQ(utils::ToIndexVectorForTests(double_res),
            utils::ToIndexVectorForTests(int_res));

  op = FilterOp::kGe;
  double_res = storage.Search(op, double_val, range);
  int_res = storage.Search(op, long_val, range);
  ASSERT_EQ(utils::ToIndexVectorForTests(double_res),
            utils::ToIndexVectorForTests(int_res));

  op = FilterOp::kGt;
  double_res = storage.Search(op, double_val, range);
  int_res = storage.Search(op, long_val, range);
  ASSERT_EQ(utils::ToIndexVectorForTests(double_res),
            utils::ToIndexVectorForTests(int_res));
}

TEST(SetIdStorage, SearchWithIdAsDouble) {
  std::vector<uint32_t> storage_data{0, 0, 0, 3, 3, 3, 6, 6, 6, 9, 9, 9};
  SetIdStorage storage(&storage_data);
  SqlValue val = SqlValue::Double(7.5);
  Range range(5, 10);

  Range res = storage.Search(FilterOp::kEq, val, range).TakeIfRange();
  ASSERT_EQ(res, Range());

  res = storage.Search(FilterOp::kNe, val, range).TakeIfRange();
  ASSERT_EQ(res, Range(0, 10));

  res = storage.Search(FilterOp::kLe, val, range).TakeIfRange();
  ASSERT_EQ(res, Range(5, 9));

  res = storage.Search(FilterOp::kLt, val, range).TakeIfRange();
  ASSERT_EQ(res, Range(5, 9));

  res = storage.Search(FilterOp::kGe, val, range).TakeIfRange();
  ASSERT_EQ(res, Range(9, 10));

  res = storage.Search(FilterOp::kGt, val, range).TakeIfRange();
  ASSERT_EQ(res, Range(9, 10));
}

}  // namespace
}  // namespace column
}  // namespace trace_processor
}  // namespace perfetto
