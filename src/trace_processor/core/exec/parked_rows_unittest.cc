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

#include "src/trace_processor/core/exec/parked_rows.h"

#include <cstdint>
#include <memory>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using testing::ElementsAre;

// A batch of one borrowed Int64 column.
RowBatch Batch(const std::vector<int64_t>& values) {
  RowBatch batch;
  batch.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Int64{}}, values.data()));
  batch.SetRowCount(static_cast<uint32_t>(values.size()));
  return batch;
}

// Rows kept are numbered in their store in the order they are parked, across
// batches. Rows dropped before parking are not in the store.
TEST(ParkedRowsTest, NumbersTheRowsKeptAcrossBatches) {
  Context context;
  ParkRows park(ParkRowsSpec{0, {0}});
  std::unique_ptr<OperatorState> state = park.MakeState(context);

  std::vector<int64_t> first = {10, 20, 30};
  RowBatch batch = Batch(first);
  uint32_t kept[] = {0, 2};
  batch.mutable_selection().Keep(Span<const uint32_t>(kept, kept + 2));
  ASSERT_TRUE(park.Process(batch, *state));
  ASSERT_EQ(batch.column_count(), 2u);
  EXPECT_THAT(test::ReadColumn<uint32_t>(batch, 1), ElementsAre(0, 1));

  std::vector<int64_t> second = {40};
  batch = Batch(second);
  ASSERT_TRUE(park.Process(batch, *state));
  EXPECT_THAT(test::ReadColumn<uint32_t>(batch, 1), ElementsAre(2));
  EXPECT_EQ(context.Store(0).size(), 3u);
}

// Fetching reads each row's values back by its number, in any order and with
// repeats.
TEST(ParkedRowsTest, FetchesByNumber) {
  Context context;
  ParkRows park(ParkRowsSpec{0, {0}});
  std::unique_ptr<OperatorState> park_state = park.MakeState(context);
  std::vector<int64_t> values = {10, 20, 30};
  RowBatch parked = Batch(values);
  ASSERT_TRUE(park.Process(parked, *park_state));

  std::vector<uint32_t> numbers = {2, 0, 2, 1};
  RowBatch batch;
  batch.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Uint32{}}, numbers.data()));
  batch.SetRowCount(4);
  FetchRows fetch(FetchRowsSpec{0, 0, {0}});
  std::unique_ptr<OperatorState> fetch_state = fetch.MakeState(context);
  ASSERT_TRUE(fetch.Process(batch, *fetch_state));
  EXPECT_THAT(test::ReadColumn<int64_t>(batch, 1), ElementsAre(30, 10, 30, 20));
}

// A batch keeping no rows still gets the columns, holding what they hold.
TEST(ParkedRowsTest, FetchingIntoABatchKeepingNoRowsAddsTheColumns) {
  Context context;
  ParkRows park(ParkRowsSpec{0, {0}});
  std::unique_ptr<OperatorState> park_state = park.MakeState(context);
  std::vector<int64_t> values = {10};
  RowBatch parked = Batch(values);
  parked.mutable_selection().KeepRange(0, 0);
  ASSERT_TRUE(park.Process(parked, *park_state));

  FetchRows fetch(FetchRowsSpec{0, 1, {0}});
  std::unique_ptr<OperatorState> fetch_state = fetch.MakeState(context);
  ASSERT_TRUE(fetch.Process(parked, *fetch_state));
  ASSERT_EQ(parked.column_count(), 3u);
  EXPECT_TRUE(parked.column(2).type().Is<Int64>());
}

// Every run starts with an empty store, so its rows are numbered from 0.
TEST(ParkedRowsTest, EachRunStartsWithAnEmptyStore) {
  Context context;
  ParkRows park(ParkRowsSpec{0, {0}});
  std::vector<int64_t> values = {10, 20};
  for (int run = 0; run < 2; ++run) {
    std::unique_ptr<OperatorState> state = park.MakeState(context);
    RowBatch batch = Batch(values);
    ASSERT_TRUE(park.Process(batch, *state));
    EXPECT_THAT(test::ReadColumn<uint32_t>(batch, 1), ElementsAre(0, 1));
  }
}

TEST(ParkedRowsTest, SelectColumnsLaysTheColumnsOutAgain) {
  std::vector<int64_t> a = {1, 2};
  std::vector<int64_t> b = {3, 4};
  RowBatch in = Batch(a);
  in.AddBorrowedColumn(ColumnView::Reference(StorageType{Int64{}}, b.data()));
  SelectColumns select({1, 0, 1});
  std::unique_ptr<OperatorState> state = select.MakeState(test::TestContext());
  RowBatch out;
  ASSERT_EQ(select.Execute(in, out, *state), OpResult::kNeedMoreInput);
  ASSERT_EQ(out.column_count(), 3u);
  EXPECT_THAT(test::ReadColumn<int64_t>(out, 0), ElementsAre(3, 4));
  EXPECT_THAT(test::ReadColumn<int64_t>(out, 1), ElementsAre(1, 2));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
