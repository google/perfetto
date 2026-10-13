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
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/exec/variant.h"
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

TEST(RowStoreTest, KeepsWhatSeveralBatchesCarried) {
  std::vector<int64_t> values = {10, 11, 12, 13, 14};
  RowStore store;
  RowBatch batch;
  Fill(&batch, values, 0, 2);
  ASSERT_TRUE(store.Append(batch).ok());
  Fill(&batch, values, 2, 3);
  ASSERT_TRUE(store.Append(batch).ok());

  EXPECT_EQ(store.size(), 5u);
  EXPECT_THAT(ReadAll(store, 0), ElementsAre(10, 11, 12, 13, 14));
}

// Holding a batch holds the buffers it is onto, so whoever filled them cannot
// be handed them again to fill with the next batch.
TEST(RowStoreTest, HoldsTheBuffersOfWhatItKeeps) {
  RowStore store;
  RowBatch batch;
  FillBuffer(&batch, {1, 2, 3});
  ASSERT_TRUE(store.Append(batch).ok());

  FillBuffer(&batch, {99, 99, 99});
  EXPECT_THAT(ReadAll(store, 0), ElementsAre(1, 2, 3));
}

// A view of what the store holds holds the same buffers, after the store lets
// go of them too.
TEST(RowStoreTest, AViewStillHeldSurvivesClearingAndRefilling) {
  RowStore store;
  RowBatch batch;
  FillBuffer(&batch, {1, 2, 3});
  ASSERT_TRUE(store.Append(batch).ok());
  RowBatch held;
  store.View(&held, 0, 3);

  store.Clear();
  FillBuffer(&batch, {7, 8, 9});
  ASSERT_TRUE(store.Append(batch).ok());

  EXPECT_THAT(test::ReadColumn<int64_t>(held, 0), ElementsAre(1, 2, 3));
  EXPECT_THAT(ReadAll(store, 0), ElementsAre(7, 8, 9));
}

TEST(RowStoreTest, RefillsAfterClearing) {
  std::vector<int64_t> first = {1, 2, 3};
  std::vector<int64_t> second = {7, 8};
  RowStore store;
  RowBatch batch;
  Fill(&batch, first, 0, 3);
  ASSERT_TRUE(store.Append(batch).ok());
  store.Clear();
  Fill(&batch, second, 0, 2);
  ASSERT_TRUE(store.Append(batch).ok());

  EXPECT_THAT(ReadAll(store, 0), ElementsAre(7, 8));
}

// A batch keeping only some of its rows hands back only those.
TEST(RowStoreTest, ReadsBackOnlyTheRowsKept) {
  std::vector<int64_t> values = {10, 11, 12, 13, 14};
  RowStore store;
  RowBatch batch;
  batch.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Int64{}}, values.data()));
  batch.SetRowCount(5);
  std::vector<uint32_t> kept = {0, 2, 4};
  batch.mutable_selection().Keep(kept);
  ASSERT_TRUE(store.Append(batch).ok());

  EXPECT_THAT(ReadAll(store, 0), ElementsAre(10, 12, 14));
}

TEST(RowStoreTest, HandsBackARunOfItsRows) {
  std::vector<int64_t> values = {10, 11, 12, 13, 14};
  RowStore store;
  RowBatch batch;
  Fill(&batch, values, 0, 5);
  ASSERT_TRUE(store.Append(batch).ok());

  RowBatch out;
  EXPECT_EQ(store.View(&out, 2, 3), 3u);
  EXPECT_EQ(out.size(), 3u);
  EXPECT_THAT(test::ReadColumn<int64_t>(out, 0), ElementsAre(12, 13, 14));
}

TEST(RowStoreTest, HandsBackTheRowsAnOrderPicksOut) {
  std::vector<int64_t> values = {10, 11, 12, 13, 14};
  std::vector<uint32_t> order = {4, 3, 0};
  RowStore store;
  RowBatch batch;
  Fill(&batch, values, 0, 5);
  ASSERT_TRUE(store.Append(batch).ok());

  RowBatch out;
  Gather(store, &out, order);
  EXPECT_THAT(test::ReadColumn<int64_t>(out, 0), ElementsAre(14, 13, 10));
}

// Viewing twice must not accumulate columns in the batch.
TEST(RowStoreTest, AViewReplacesTheOneBefore) {
  std::vector<int64_t> values = {10, 11};
  RowStore store;
  RowBatch batch;
  Fill(&batch, values, 0, 2);
  ASSERT_TRUE(store.Append(batch).ok());

  RowBatch out;
  store.View(&out, 0, 2);
  store.View(&out, 0, 2);
  EXPECT_EQ(out.column_count(), 1u);
}

TEST(RowStoreTest, KeepsWhichRowsHeldNothing) {
  std::vector<int64_t> values = {10, 11, 12};
  BitVector validity = BitVector::CreateWithSize(3);
  validity.set(0);
  validity.set(2);
  RowStore store;
  RowBatch batch;
  batch.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Int64{}}, values.data(), &validity));
  test::Window(&batch, 0, 3);
  ASSERT_TRUE(store.Append(batch).ok());

  RowBatch out;
  store.View(&out, 0, 3);
  const BitVector* kept = out.column(0).validity();
  ASSERT_NE(kept, nullptr);
  EXPECT_TRUE(kept->is_set(0));
  EXPECT_FALSE(kept->is_set(1));
  EXPECT_TRUE(kept->is_set(2));
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

TEST(RowStoreTest, GatheringAcrossBatchesKeepsNulls) {
  std::vector<int64_t> values = {10, 11, 12};
  RowStore store;
  RowBatch batch;
  Fill(&batch, values, 0, 3);
  ASSERT_TRUE(store.Append(batch).ok());

  std::vector<int64_t> more = {20, 21};
  BitVector validity = BitVector::CreateWithSize(2);
  validity.set(0);
  batch.Reset();
  batch.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Int64{}}, more.data(), &validity));
  test::Window(&batch, 0, 2);
  ASSERT_TRUE(store.Append(batch).ok());

  RowBatch out;
  std::vector<uint32_t> order = {0, 3, 1, 4, 2};
  ASSERT_EQ(Gather(store, &out, order), 5u);
  EXPECT_THAT(test::ReadNullableColumn<int64_t>(out, 0),
              ElementsAre(10, 20, 11, std::nullopt, 12));
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
}

TEST(RowStoreTest, AZeroColumnBatchFixesTheSchema) {
  RowStore store;
  RowBatch empty_schema;
  empty_schema.SetRowCount(2);
  ASSERT_TRUE(store.Append(empty_schema).ok());

  std::vector<int64_t> values = {1};
  RowBatch with_column;
  Fill(&with_column, values, 0, 1);
  EXPECT_FALSE(store.Append(with_column).ok());
  EXPECT_EQ(store.size(), 2u);
  RowBatch out;
  ASSERT_EQ(store.View(&out, 0, 2), 2u);
  EXPECT_EQ(out.column_count(), 0u);
}

TEST(RowStoreTest, ViewingAnEmptySuffixReturnsAnEmptyBatch) {
  std::vector<int64_t> values(kMaxBatchRows);
  RowStore store;
  RowBatch batch;
  Fill(&batch, values, 0, kMaxBatchRows);
  ASSERT_TRUE(store.Append(batch).ok());

  RowBatch out;
  EXPECT_EQ(store.View(&out, store.size(), 0), 0u);
  EXPECT_EQ(out.size(), 0u);
}

// An Id column has no storage: its value is the row it sits at, so gathering
// one means writing those rows out.
TEST(RowStoreTest, AnIdColumnBecomesTheRowsItStoodFor) {
  RowStore store;
  RowBatch batch;
  batch.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Id{}}, nullptr, nullptr));
  test::Window(&batch, 7, 3);
  ASSERT_TRUE(store.Append(batch).ok());

  RowBatch out;
  Gather(store, &out, {0, 1, 2});
  ASSERT_TRUE(out.column(0).type().Is<Uint32>());
  const auto* data = static_cast<const uint32_t*>(out.column(0).data());
  EXPECT_THAT(std::vector<uint32_t>(data, data + 3), ElementsAre(7u, 8u, 9u));
}

TEST(RowStoreTest, ABatchOfADifferentShapeIsReported) {
  std::vector<int64_t> values = {1, 2};
  RowStore store;
  RowBatch batch;
  Fill(&batch, values, 0, 2);
  ASSERT_TRUE(store.Append(batch).ok());

  RowBatch wider;
  wider.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Int64{}}, values.data()));
  wider.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Int64{}}, values.data()));
  test::Window(&wider, 0, 2);
  EXPECT_FALSE(store.Append(wider).ok());
}

// Rows held are never copied: the run a given row sits in keeps its address
// however many batches arrive after it.
TEST(RowStoreTest, AppendingDoesNotMoveRetainedStorage) {
  const uint32_t kRows = 200000, kChunk = 2048;
  std::vector<int64_t> values(kChunk);
  RowStore store;
  RowBatch batch;
  RowBatch first;
  for (uint32_t done = 0; done < kRows; done += kChunk) {
    Fill(&batch, values, 0, kChunk);
    ASSERT_TRUE(store.Append(batch).ok());
    if (done == 0) {
      store.View(&first, 0, kChunk);
    }
  }
  RowBatch again;
  store.View(&again, 0, kChunk);
  EXPECT_EQ(again.column(0).data(), first.column(0).data());
}

// A run never spans two batches held, so a caller asking for more than the
// rest of one gets the rest of it and comes back for the next.
TEST(RowStoreTest, ARunStopsAtTheEndOfAnInputBatch) {
  std::vector<int64_t> values(kMaxBatchRows);
  RowStore store;
  RowBatch batch;
  Fill(&batch, values, 0, kMaxBatchRows);
  ASSERT_TRUE(store.Append(batch).ok());
  ASSERT_TRUE(store.Append(batch).ok());

  RowBatch out;
  EXPECT_EQ(store.View(&out, 1, store.size() - 1), kMaxBatchRows - 1);
  EXPECT_EQ(store.View(&out, kMaxBatchRows, store.size() - kMaxBatchRows),
            kMaxBatchRows);
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

// A gather can name rows from any input batch.
TEST(RowStoreTest, GathersRowsFromSeveralInputBatches) {
  const uint32_t kRows = kMaxBatchRows * 2 + 5;
  std::vector<int64_t> values(kMaxBatchRows);
  for (uint32_t i = 0; i < values.size(); ++i) {
    values[i] = static_cast<int64_t>(i);
  }
  RowStore store;
  RowBatch batch;
  std::vector<int64_t> all;
  for (uint32_t done = 0; done < kRows;) {
    uint32_t n = std::min(kMaxBatchRows, kRows - done);
    Fill(&batch, values, 0, n);
    ASSERT_TRUE(store.Append(batch).ok());
    all.insert(all.end(), values.begin(), values.begin() + n);
    done += n;
  }
  std::vector<uint32_t> order = {kRows - 1, 0, kMaxBatchRows, kMaxBatchRows - 1,
                                 3};
  RowBatch out;
  Gather(store, &out, order);
  std::vector<int64_t> expected;
  for (uint32_t row : order) {
    expected.push_back(all[row]);
  }
  EXPECT_EQ(test::ReadColumn<int64_t>(out, 0), expected);
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

TEST(RowStoreTest, KeepsAColumnWhoseTypeIsPerRow) {
  StringPool pool;
  std::vector<Variant> values = {Variant::Int64(7), Variant::Null(),
                                 Variant::Double(1.5),
                                 Variant::String(pool.InternString("hi"))};
  RowBatch batch;
  batch.AddBorrowedColumn(ColumnView::Variants(values.data()));
  test::Window(&batch, 0, 4);

  RowStore store;
  ASSERT_TRUE(store.Append(batch).ok());

  RowBatch out;
  ASSERT_EQ(store.View(&out, 0, 4), 4u);
  ASSERT_EQ(out.column(0).kind(), ColumnView::Kind::kVariant);
  const auto* kept = static_cast<const Variant*>(out.column(0).data());
  EXPECT_EQ(kept[0].AsInt64(), 7);
  EXPECT_EQ(kept[1].type, Variant::Type::kNull);
  EXPECT_EQ(kept[2].AsDouble(), 1.5);
  EXPECT_EQ(pool.Get(kept[3].AsString()).ToStdString(), "hi");
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
