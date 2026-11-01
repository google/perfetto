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
#include "perfetto/ext/base/span.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/util/bit_vector.h"
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
  ColumnBuffer buffer = test::TestContext().TakeBuffer(sizeof(int64_t));
  int64_t* data = buffer.chunk().Values<int64_t>();
  std::copy(values.begin(), values.end(), data);
  batch->AddColumn(ColumnView::Reference(StorageType{Int64{}}, data),
                   std::move(buffer));
  batch->SetRowCount(static_cast<uint32_t>(values.size()));
}

// Gathers `rows` of the store.
uint32_t Gather(RowStore& store, RowBatch* out, std::vector<uint32_t> rows) {
  return store.Gather(
      out, base::Span<const uint32_t>(rows.data(), rows.data() + rows.size()),
      test::TestContext());
}

// Reads every row of the store back, a run at a time.
std::vector<int64_t> ReadAll(RowStore& store, uint32_t column);

std::vector<int64_t> ReadAll(RowStore& store, uint32_t column) {
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
  ASSERT_TRUE(store.Append(batch, test::TestContext()).ok());

  BitVector validity = BitVector::CreateWithSize(1);
  int64_t null_value = 0;
  batch.Reset();
  batch.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Int64{}}, &null_value, &validity));
  batch.SetRowCount(1);
  ASSERT_TRUE(store.Append(batch, test::TestContext()).ok());

  RowBatch out;
  ASSERT_EQ(store.View(&out, 0, kMaxBatchRows), kMaxBatchRows);
  EXPECT_EQ(test::ReadNullableColumn<int64_t>(out, 0),
            std::vector<std::optional<int64_t>>(kMaxBatchRows, 1));
  ASSERT_EQ(store.View(&out, kMaxBatchRows, 1), 1u);
  EXPECT_FALSE(out.column(0).validity()->is_set(0));
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
      ASSERT_TRUE(store.Append(batch, test::TestContext()).ok());
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
    ASSERT_TRUE(store.Append(input, test::TestContext()).ok());
    ASSERT_EQ(store.View(&range, 0, 3), 3u);
    Gather(store, &gathered, {2, 0, 2});
    EXPECT_TRUE(gathered.selection().prefix());
    Gather(store, &later, {1, 1, 0});
    store.Clear();
    FillBuffer(&input, {40, 50, 60});
    ASSERT_TRUE(store.Append(input, test::TestContext()).ok());
  }
  EXPECT_THAT(test::ReadColumn<int64_t>(range, 0), ElementsAre(10, 20, 30));
  EXPECT_THAT(test::ReadColumn<int64_t>(gathered, 0), ElementsAre(30, 10, 30));
}

// Rows gathered from several batches keep their values and their nulls,
// whichever batch they come from and however the batches' windows start.
TEST(RowStoreTest, GathersAcrossBatchesWithAndWithoutNulls) {
  std::vector<int64_t> values = {0, 1, 2, 3, 4};
  RowStore store;
  RowBatch batch;
  Fill(&batch, values, 1, 3);
  ASSERT_TRUE(store.Append(batch, test::TestContext()).ok());

  std::vector<int64_t> more = {100, 200, 300};
  BitVector validity = BitVector::CreateWithSize(3);
  validity.set(1);
  batch.Reset();
  batch.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Int64{}}, more.data(), &validity, 1));
  batch.SetRowCount(2);
  ASSERT_TRUE(store.Append(batch, test::TestContext()).ok());

  RowBatch out;
  ASSERT_EQ(Gather(store, &out, {4, 0, 3, 2, 3}), 5u);
  EXPECT_THAT(test::ReadNullableColumn<int64_t>(out, 0),
              ElementsAre(std::nullopt, 1, 200, 3, 200));
}

// A batch keeping most of a batch's worth of rows is held as it is.
TEST(RowStoreTest, SharesTheBuffersOfALargeBatch) {
  std::vector<int64_t> values(kMaxBatchRows);
  for (uint32_t i = 0; i < kMaxBatchRows; ++i) {
    values[i] = i;
  }
  RowStore store;
  RowBatch input;
  FillBuffer(&input, values);
  const void* data = input.column(0).data();
  ASSERT_TRUE(store.Append(input, test::TestContext()).ok());
  RowBatch out;
  ASSERT_EQ(store.View(&out, 0, kMaxBatchRows), kMaxBatchRows);
  EXPECT_EQ(out.column(0).data(), data);
}

// A batch whose columns all borrow storage which outlives the run costs
// nothing to hold, however few rows it keeps.
TEST(RowStoreTest, SharesASmallBatchOfBorrowedColumns) {
  std::vector<int64_t> values = {1, 2, 3};
  RowStore store;
  RowBatch input;
  Fill(&input, values, 0, 3);
  ASSERT_TRUE(store.Append(input, test::TestContext()).ok());
  RowBatch out;
  ASSERT_EQ(store.View(&out, 0, 3), 3u);
  EXPECT_EQ(out.column(0).data(), values.data());
}

// A few rows of a batch with buffers of its own are copied, so the store
// does not keep the batch's buffers alive. Rows packed and rows shared come
// back in the order they were appended.
TEST(RowStoreTest, PacksAFewRowsOfABatchWithBuffers) {
  std::vector<int64_t> large(kMaxBatchRows, 7);
  RowStore store;
  RowBatch input;
  FillBuffer(&input, {10, 20, 30});
  const void* data = input.column(0).data();
  input.mutable_selection().KeepRange(1, 2);
  ASSERT_TRUE(store.Append(input, test::TestContext()).ok());
  FillBuffer(&input, large);
  ASSERT_TRUE(store.Append(input, test::TestContext()).ok());
  FillBuffer(&input, {40});
  ASSERT_TRUE(store.Append(input, test::TestContext()).ok());
  ASSERT_EQ(store.size(), kMaxBatchRows + 3);

  RowBatch out;
  ASSERT_EQ(store.View(&out, 0, 2), 2u);
  EXPECT_NE(out.column(0).data(), data);
  EXPECT_THAT(test::ReadColumn<int64_t>(out, 0), ElementsAre(20, 30));
  EXPECT_THAT(ReadAll(store, 0).back(), 40);
  RowBatch gathered;
  ASSERT_EQ(Gather(store, &gathered, {kMaxBatchRows + 2, 0, 5}), 3u);
  EXPECT_THAT(test::ReadColumn<int64_t>(gathered, 0), ElementsAre(40, 20, 7));
}

// Rows asked for in one ascending run of one batch held are viewed where
// they are held, not copied.
TEST(RowStoreTest, GathersARunOfOneBatchWithoutCopying) {
  std::vector<int64_t> values(kMaxBatchRows);
  for (uint32_t i = 0; i < kMaxBatchRows; ++i) {
    values[i] = i;
  }
  RowStore store;
  RowBatch input;
  FillBuffer(&input, values);
  const auto* data = static_cast<const int64_t*>(input.column(0).data());
  ASSERT_TRUE(store.Append(input, test::TestContext()).ok());
  RowBatch out;
  ASSERT_EQ(Gather(store, &out, {5, 6, 7}), 3u);
  EXPECT_EQ(
      static_cast<const int64_t*>(out.column(0).data()) + out.column(0).start(),
      data + 5);
  EXPECT_THAT(test::ReadColumn<int64_t>(out, 0), ElementsAre(5, 6, 7));
}

// A gather turns an implicit id into a stored one, so a stream can carry both.
// A column never read back is not held, so its buffer is given back at once,
// and it reads back as nulls.
TEST(RowStoreTest, DoesNotHoldColumnsNeverReadBack) {
  Context context;
  std::vector<int64_t> kept(kMaxBatchRows);
  for (uint32_t i = 0; i < kMaxBatchRows; ++i) {
    kept[i] = i;
  }
  RowStore store;
  store.SetUnread({false, true});
  const ColumnChunk* unread;
  {
    ColumnBuffer buffer = context.TakeBuffer(sizeof(int64_t));
    unread = &buffer.chunk();
    RowBatch input;
    input.AddBorrowedColumn(
        ColumnView::Reference(StorageType{Int64{}}, kept.data()));
    // Made before the buffer moves: arguments are evaluated in any order.
    ColumnView view = ColumnView::Reference(StorageType{Int64{}},
                                            buffer.chunk().Values<int64_t>());
    input.AddColumn(view, std::move(buffer));
    input.SetRowCount(kMaxBatchRows);
    ASSERT_TRUE(store.Append(input, context).ok());
  }
  EXPECT_EQ(&context.TakeBuffer(sizeof(int64_t)).chunk(), unread);

  RowBatch out;
  ASSERT_EQ(store.View(&out, 1, 2), 2u);
  EXPECT_THAT(test::ReadColumn<int64_t>(out, 0), ElementsAre(1, 2));
  EXPECT_THAT(test::ReadNullableColumn<int64_t>(out, 1),
              ElementsAre(std::nullopt, std::nullopt));
  RowBatch gathered;
  ASSERT_EQ(Gather(store, &gathered, {5, 0}), 2u);
  EXPECT_THAT(test::ReadColumn<int64_t>(gathered, 0), ElementsAre(5, 0));
  EXPECT_THAT(test::ReadNullableColumn<int64_t>(gathered, 1),
              ElementsAre(std::nullopt, std::nullopt));
}

TEST(RowStoreTest, AnImplicitIdColumnCanBecomeAStoredOne) {
  auto stored =
      std::make_shared<std::vector<uint32_t>>(std::vector<uint32_t>{40, 41});
  RowStore store;
  RowBatch batch;
  batch.AddBorrowedColumn(ColumnView::Reference(StorageType{Id{}}, nullptr));
  test::Window(&batch, 5, 2);
  ASSERT_TRUE(store.Append(batch, test::TestContext()).ok());

  batch.Reset();
  batch.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Uint32{}}, stored->data()));
  batch.SetRowCount(2);
  base::Status status = store.Append(batch, test::TestContext());
  ASSERT_TRUE(status.ok()) << status.message();

  RowBatch out;
  ASSERT_EQ(Gather(store, &out, {3, 0, 2, 1}), 4u);
  EXPECT_THAT(test::ReadColumn<uint32_t>(out, 0), ElementsAre(41, 5, 40, 6));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
