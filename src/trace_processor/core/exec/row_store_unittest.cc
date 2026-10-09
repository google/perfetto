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

#include "src/trace_processor/core/exec/row_store.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/span.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using testing::ElementsAre;

// Points a batch at a single int64 column, the way a source would.
void Fill(RowBatch* batch,
          const std::vector<int64_t>& values,
          uint32_t offset,
          uint32_t count) {
  batch->Reset();
  batch->AddBorrowedColumn(
      ColumnView::Reference(StorageType{Int64{}}, values.data()));
  test::Window(batch, offset, count);
}

// Fills a batch the way an operator computing a column does: into a buffer
// of its own, taken afresh for each batch.
void FillBuffer(RowBatch* batch, const std::vector<int64_t>& values) {
  batch->Reset();
  ColumnBuffer buffer = test::TestContext().TakeBuffer();
  int64_t* data = buffer.chunk().Values<int64_t>();
  std::copy(values.begin(), values.end(), data);
  batch->AddColumn(ColumnView::Reference(StorageType{Int64{}}, data),
                   std::move(buffer));
  batch->SetRowCount(static_cast<uint32_t>(values.size()));
}

// Gathers `rows` of the store.
uint32_t Gather(RowStore& store, RowBatch* out, std::vector<uint32_t> rows) {
  return store.View(
      out, Span<const uint32_t>(rows.data(), rows.data() + rows.size()),
      test::TestContext());
}

// Reads every row of the store back, a run at a time.
std::vector<int64_t> ReadAll(const RowStore& store, uint32_t column);

std::vector<int64_t> ReadAll(const RowStore& store, uint32_t column) {
  RowBatch batch;
  std::vector<int64_t> out;
  for (uint32_t at = 0; at < store.size();) {
    at += store.View(&batch, at, store.size() - at);
    std::vector<int64_t> run = test::ReadColumn<int64_t>(batch, column);
    out.insert(out.end(), run.begin(), run.end());
  }
  return out;
}

TEST(RowStoreTest, RetainsNonNullBatchesWhenLaterBatchesAreNullable) {
  std::vector<int64_t> values(kMaxBatchRows, 1);
  RowStore store;
  RowBatch batch;
  Fill(&batch, values, 0, kMaxBatchRows);
  ASSERT_TRUE(store.Append(batch).ok());

  BitVector validity = BitVector::CreateWithSize(1);
  int64_t null_value = 0;
  batch.Reset();
  batch.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Int64{}}, &null_value, &validity));
  batch.SetRowCount(1);
  ASSERT_TRUE(store.Append(batch).ok());

  RowBatch out;
  ASSERT_EQ(store.View(&out, 0, kMaxBatchRows), kMaxBatchRows);
  EXPECT_EQ(test::ReadNullableColumn<int64_t>(out, 0),
            std::vector<std::optional<int64_t>>(kMaxBatchRows, 1));
  ASSERT_EQ(store.View(&out, kMaxBatchRows, 1), 1u);
  EXPECT_FALSE(out.column(0).validity()->is_set(0));
}

TEST(RowStoreTest, RejectsABatchBeforeMutatingAnyColumn) {
  std::vector<int64_t> ints = {1, 2};
  std::vector<double> doubles = {1.0, 2.0};
  RowStore store;
  RowBatch batch;
  batch.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Int64{}}, ints.data()));
  batch.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Int64{}}, ints.data()));
  batch.SetRowCount(1);
  ASSERT_TRUE(store.Append(batch).ok());

  BitVector validity = BitVector::CreateWithSize(2);
  validity.set(1);
  batch.Reset();
  batch.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Int64{}}, ints.data(), &validity));
  batch.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Double{}}, doubles.data()));
  test::Window(&batch, 1, 1);
  EXPECT_FALSE(store.Append(batch).ok());

  RowBatch out;
  ASSERT_EQ(store.View(&out, 0, 1), 1u);
  EXPECT_EQ(out.column(0).validity(), nullptr);
  EXPECT_EQ(store.size(), 1u);

  // Nor can a batch of more columns than those before it.
  RowBatch wider;
  for (uint32_t i = 0; i < 3; ++i) {
    wider.AddBorrowedColumn(
        ColumnView::Reference(StorageType{Int64{}}, ints.data()));
  }
  wider.SetRowCount(1);
  EXPECT_FALSE(store.Append(wider).ok());

  // A batch of no columns fixes that there are none.
  RowStore empty;
  RowBatch no_columns;
  no_columns.SetRowCount(2);
  ASSERT_TRUE(empty.Append(no_columns).ok());
  EXPECT_FALSE(empty.Append(wider).ok());
  ASSERT_EQ(empty.View(&out, 0, 2), 2u);
  EXPECT_EQ(out.column_count(), 0u);
}

// Input batch boundaries do not change the logical sequence of stored rows.
TEST(RowStoreTest, InputBatchSizesDoNotChangeTheRows) {
  const uint32_t kRows = kMaxBatchRows * 2 + 37;
  std::vector<int64_t> values(kMaxBatchRows);
  for (uint32_t i = 0; i < values.size(); ++i) {
    values[i] = 1000 + i;
  }
  for (uint32_t batch_rows : {1u, 7u, 700u, kMaxBatchRows}) {
    RowStore store;
    RowBatch batch;
    std::vector<int64_t> expected;
    for (uint32_t done = 0; done < kRows;) {
      uint32_t n = std::min(batch_rows, kRows - done);
      Fill(&batch, values, 0, n);
      ASSERT_TRUE(store.Append(batch).ok());
      for (uint32_t i = 0; i < n; ++i) {
        expected.push_back(values[i]);
      }
      done += n;
    }
    ASSERT_EQ(store.size(), kRows) << "batch of " << batch_rows;
    EXPECT_EQ(ReadAll(store, 0), expected) << "batch of " << batch_rows;
  }
}

TEST(RowStoreTest, RetainedViewsSurviveGatherClearAndDestruction) {
  RowBatch range, gathered;
  {
    RowStore store;
    RowBatch input, later;
    FillBuffer(&input, {10, 20, 30});
    const void* values = input.column(0).data();
    ASSERT_TRUE(store.Append(input).ok());
    ASSERT_EQ(store.View(&range, 0, 3), 3u);
    EXPECT_EQ(range.column(0).data(), values);
    Gather(store, &gathered, {2, 0, 2});
    EXPECT_NE(gathered.column(0).data(), values);
    EXPECT_TRUE(gathered.selection().prefix());
    Gather(store, &later, {1, 1, 0});
    store.Clear();
    FillBuffer(&input, {40, 50, 60});
    ASSERT_TRUE(store.Append(input).ok());
  }
  EXPECT_THAT(test::ReadColumn<int64_t>(range, 0), ElementsAre(10, 20, 30));
  EXPECT_THAT(test::ReadColumn<int64_t>(gathered, 0), ElementsAre(30, 10, 30));
}

// A gather turns an implicit id into a stored one, so a stream can carry both.
TEST(RowStoreTest, AnImplicitIdColumnCanBecomeAStoredOne) {
  auto stored =
      std::make_shared<std::vector<uint32_t>>(std::vector<uint32_t>{40, 41});
  RowStore store;
  RowBatch batch;
  batch.AddBorrowedColumn(ColumnView::Reference(StorageType{Id{}}, nullptr));
  test::Window(&batch, 5, 2);
  ASSERT_TRUE(store.Append(batch).ok());

  batch.Reset();
  batch.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Uint32{}}, stored->data()));
  batch.SetRowCount(2);
  base::Status status = store.Append(batch);
  ASSERT_TRUE(status.ok()) << status.message();

  RowBatch out;
  ASSERT_EQ(Gather(store, &out, {3, 0, 2, 1}), 4u);
  EXPECT_THAT(test::ReadColumn<uint32_t>(out, 0), ElementsAre(41, 5, 40, 6));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
