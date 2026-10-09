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

#include "src/trace_processor/core/exec/hash_aggregate.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using test::ReadColumn;
using test::ReadNullableColumn;
using testing::ElementsAre;

TEST(HashAggregateTest, FoldsGroupsInTheOrderFirstSeen) {
  // Two batches of key, value; the second key and value columns are nullable.
  std::vector<int64_t> keys = {3, 1, 3, 2, 0, 1, 0};
  std::vector<int64_t> values = {10, 20, 30, 40, 50, 60, 70};
  BitVector key_valid = BitVector::CreateWithSize(7, true);
  key_valid.clear(4);
  key_valid.clear(6);
  BitVector value_valid = BitVector::CreateWithSize(7, true);
  value_valid.clear(3);
  value_valid.clear(5);

  using F = AggregateCall::Function;
  HashAggregate op(HashAggregateSpec{{0}, {{F::kCountStar, 0}, {F::kSum, 1}}});
  std::unique_ptr<OperatorState> state = op.MakeState(test::TestContext());
  RowBatch out;
  for (uint32_t at : {0u, 4u}) {
    uint32_t count = at == 0 ? 4 : 3;
    RowBatch in;
    in.AddBorrowedColumn(ColumnView::Reference(StorageType{Int64{}},
                                               keys.data(), &key_valid, at));
    in.AddBorrowedColumn(ColumnView::Reference(
        StorageType{Int64{}}, values.data(), &value_valid, at));
    in.SetRowCount(count);
    ASSERT_EQ(op.Execute(in, out, *state), OpResult::kNeedMoreInput);
  }
  ASSERT_EQ(op.Finish(out, *state), OpResult::kHaveMoreOutput);
  // Groups 3, 1, 2 and the null key; a sum of only nulls is null.
  EXPECT_THAT(ReadNullableColumn<int64_t>(out, 0),
              ElementsAre(3, 1, 2, std::nullopt));
  EXPECT_THAT(ReadColumn<int64_t>(out, 1), ElementsAre(2, 2, 1, 2));
  EXPECT_THAT(ReadNullableColumn<int64_t>(out, 2),
              ElementsAre(40, 20, std::nullopt, 120));
  EXPECT_EQ(op.Finish(out, *state), OpResult::kNeedMoreInput);
}

TEST(HashAggregateTest, NoKeysIsOneGroupEvenWithoutRows) {
  using F = AggregateCall::Function;
  HashAggregate op(HashAggregateSpec{{}, {{F::kCountStar, 0}, {F::kSum, 0}}});
  std::unique_ptr<OperatorState> state = op.MakeState(test::TestContext());
  RowBatch out;
  // Without rows: a count of 0 and a sum of nothing, which is null.
  ASSERT_EQ(op.Finish(out, *state), OpResult::kHaveMoreOutput);
  EXPECT_THAT(ReadColumn<int64_t>(out, 0), ElementsAre(0));
  EXPECT_THAT(ReadNullableColumn<int64_t>(out, 1), ElementsAre(std::nullopt));
  EXPECT_EQ(op.Finish(out, *state), OpResult::kNeedMoreInput);

  state->Reset();
  std::vector<int64_t> values = {4, 5, 6};
  RowBatch in;
  in.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Int64{}}, values.data()));
  in.SetRowCount(3);
  ASSERT_EQ(op.Execute(in, out, *state), OpResult::kNeedMoreInput);
  ASSERT_EQ(op.Finish(out, *state), OpResult::kHaveMoreOutput);
  EXPECT_THAT(ReadColumn<int64_t>(out, 0), ElementsAre(3));
  EXPECT_THAT(ReadNullableColumn<int64_t>(out, 1), ElementsAre(15));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
