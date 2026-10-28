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

#include "src/trace_processor/core/exec/distinct.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/span.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;

// Runs batches of one Int64 column through a Distinct over it.
struct Batches {
  Batches() : op({0}), state(op.MakeState(test::TestContext())) {}

  // The values kept from `values`, in order.
  std::vector<int64_t> Process(std::vector<int64_t> values) {
    batches.push_back(std::move(values));
    RowBatch in;
    in.AddBorrowedColumn(
        ColumnView::Reference(StorageType{Int64{}}, batches.back().data()));
    in.SetRowCount(static_cast<uint32_t>(batches.back().size()));
    EXPECT_TRUE(test::ProcessCopy(op, in, out, *state))
        << op.status(*state).message();
    return test::ReadColumn<int64_t>(out, 0);
  }

  Distinct op;
  std::unique_ptr<OperatorState> state;
  // Kept alive, as the batches borrow them.
  std::vector<std::vector<int64_t>> batches;
  RowBatch out;
};

TEST(DistinctTest, KeepsTheFirstOfEachValueInOrder) {
  Batches run;
  EXPECT_THAT(run.Process({3, 1, 3, 2, 1, 2}), ElementsAre(3, 1, 2));
}

TEST(DistinctTest, ValuesSeenInEarlierBatchesAreDropped) {
  Batches run;
  EXPECT_THAT(run.Process({1, 2}), ElementsAre(1, 2));
  EXPECT_THAT(run.Process({2, 3, 1, 3}), ElementsAre(3));
  EXPECT_THAT(run.Process({1, 2, 3}), IsEmpty());
}

TEST(DistinctTest, RowsAreEqualOnlyIfEveryColumnIs) {
  StringPool pool;
  StringPool::Id a = pool.InternString("a");
  StringPool::Id b = pool.InternString("b");
  std::vector<int64_t> ints = {1, 1, 1, 2, 2};
  std::vector<StringPool::Id> strings = {a, b, a, a, a};
  RowBatch in;
  in.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Int64{}}, ints.data()));
  in.AddBorrowedColumn(
      ColumnView::Reference(StorageType{String{}}, strings.data()));
  in.SetRowCount(5);

  Distinct op({0, 1});
  std::unique_ptr<OperatorState> state = op.MakeState(test::TestContext());
  RowBatch out;
  ASSERT_TRUE(test::ProcessCopy(op, in, out, *state));
  EXPECT_THAT(test::ReadColumn<int64_t>(out, 0), ElementsAre(1, 1, 2));
  EXPECT_THAT(test::ReadColumn<StringPool::Id>(out, 1), ElementsAre(a, b, a));
}

// As in SQL's DISTINCT, nulls equal each other but not zero, and 0.0 equals
// -0.0.
TEST(DistinctTest, NullsAreOneValueAndZeroesAreEqual) {
  std::vector<double> values = {0.0, 0.0, -0.0, 1.5, 0.0};
  BitVector valid = BitVector::CreateWithSize(5);
  valid.set(1);
  valid.set(2);
  valid.set(3);
  RowBatch in;
  in.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Double{}}, values.data(), &valid));
  in.SetRowCount(5);

  Distinct op({0});
  std::unique_ptr<OperatorState> state = op.MakeState(test::TestContext());
  RowBatch out;
  ASSERT_TRUE(test::ProcessCopy(op, in, out, *state));
  EXPECT_THAT(test::ReadNullableColumn<double>(out, 0),
              ElementsAre(std::nullopt, 0.0, 1.5));
}

// A batch whose earlier steps dropped rows is read through its selection.
TEST(DistinctTest, OnlyTheRowsKeptAreRead) {
  std::vector<int64_t> values = {7, 1, 7, 2, 1};
  RowBatch in;
  in.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Int64{}}, values.data()));
  in.SetRowCount(5);
  std::vector<uint32_t> kept = {1, 3, 4};
  in.mutable_selection().Keep(
      Span<const uint32_t>(kept.data(), kept.data() + kept.size()));

  Distinct op({0});
  std::unique_ptr<OperatorState> state = op.MakeState(test::TestContext());
  RowBatch out;
  ASSERT_TRUE(test::ProcessCopy(op, in, out, *state));
  EXPECT_THAT(test::ReadColumn<int64_t>(out, 0), ElementsAre(1, 2));
}

TEST(DistinctTest, AColumnChangingKindFailsTheRun) {
  std::vector<int64_t> ints = {1};
  std::vector<double> doubles = {1.0};
  RowBatch of_ints;
  of_ints.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Int64{}}, ints.data()));
  of_ints.SetRowCount(1);
  RowBatch of_doubles;
  of_doubles.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Double{}}, doubles.data()));
  of_doubles.SetRowCount(1);

  Distinct op({0});
  std::unique_ptr<OperatorState> state = op.MakeState(test::TestContext());
  RowBatch out;
  ASSERT_TRUE(test::ProcessCopy(op, of_ints, out, *state));
  EXPECT_FALSE(test::ProcessCopy(op, of_doubles, out, *state));
  EXPECT_THAT(op.status(*state).message(),
              HasSubstr("DISTINCT: key 1 must be a column holding one type"));

  // A new run starts with nothing seen.
  state->EndRun();
  EXPECT_TRUE(op.status(*state).ok());
  ASSERT_TRUE(test::ProcessCopy(op, of_doubles, out, *state));
  ASSERT_TRUE(test::ProcessCopy(op, of_doubles, out, *state));
  EXPECT_EQ(out.size(), 0u);
  state->EndRun();
  ASSERT_TRUE(test::ProcessCopy(op, of_doubles, out, *state));
  EXPECT_EQ(out.size(), 1u);
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
