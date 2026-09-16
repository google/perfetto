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
#include <optional>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
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
  batch->AddColumn(ColumnView::Reference(StorageType{Int64{}}, values.data()));
  batch->Compose(RowSelection::Range(offset), count);
  batch->SetCardinality(count);
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

// The reason a store exists: a source is free to overwrite the buffer a batch
// was pointing at.
TEST(RowStoreTest, SurvivesTheStorageItWasReadFromMovingOn) {
  std::vector<int64_t> buffer = {1, 2, 3};
  RowStore store;
  RowBatch batch;
  Fill(&batch, buffer, 0, 3);
  ASSERT_TRUE(store.Append(batch).ok());

  buffer = {99, 99, 99};
  EXPECT_THAT(ReadAll(store, 0), ElementsAre(1, 2, 3));
}

// A batch narrowed to a scattering of rows is stored as those rows laid out
// one after another, whatever the batch was pointing at.
TEST(RowStoreTest, ReadsBackDenseWhateverArrived) {
  std::vector<int64_t> values = {10, 11, 12, 13, 14};
  std::vector<uint32_t> rows = {4, 0, 2};
  RowStore store;
  RowBatch batch;
  batch.AddColumn(ColumnView::Reference(StorageType{Int64{}}, values.data()));
  batch.mutable_column(0).SetBorrowedRows(
      Span<const uint32_t>(rows.data(), rows.data() + 3));
  batch.SetCardinality(3);
  ASSERT_TRUE(store.Append(batch).ok());

  EXPECT_THAT(ReadAll(store, 0), ElementsAre(14, 10, 12));
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
  store.View(&out, Span<const uint32_t>(order.data(), order.data() + 3));
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
  batch.AddColumn(
      ColumnView::Reference(StorageType{Int64{}}, values.data(), &validity));
  batch.Compose(RowSelection::Range(0), 3);
  batch.SetCardinality(3);
  ASSERT_TRUE(store.Append(batch).ok());

  RowBatch out;
  store.View(&out, 0, 3);
  const BitVector* kept = out.column(0).validity();
  ASSERT_NE(kept, nullptr);
  EXPECT_TRUE(kept->is_set(0));
  EXPECT_FALSE(kept->is_set(1));
  EXPECT_TRUE(kept->is_set(2));
}

TEST(RowStoreTest, BackfillsEarlierChunksWhenNullabilityAppears) {
  std::vector<int64_t> values(RowStore::kChunkRows, 1);
  RowStore store;
  RowBatch batch;
  Fill(&batch, values, 0, RowStore::kChunkRows);
  ASSERT_TRUE(store.Append(batch).ok());

  BitVector validity = BitVector::CreateWithSize(1);
  int64_t null_value = 0;
  batch.Reset();
  batch.AddColumn(
      ColumnView::Reference(StorageType{Int64{}}, &null_value, &validity));
  batch.SetCardinality(1);
  ASSERT_TRUE(store.Append(batch).ok());

  RowBatch out;
  ASSERT_EQ(store.View(&out, 0, RowStore::kChunkRows), RowStore::kChunkRows);
  ASSERT_NE(out.column(0).validity(), nullptr);
  EXPECT_EQ(out.column(0).validity()->CountSetBits(), RowStore::kChunkRows);
  ASSERT_EQ(store.View(&out, RowStore::kChunkRows, 1), 1u);
  EXPECT_FALSE(out.column(0).validity()->is_set(0));
}

// When nullability appears, only the rows a chunk actually holds are valid:
// the unwritten tail of the last chunk must stay clear, or a null landing
// there cannot turn its bit off (the validity copy only ever sets bits).
TEST(RowStoreTest, ANullLandingInAPartlyFilledChunkStaysNull) {
  std::vector<int64_t> values = {10, 11, 12};
  RowStore store;
  RowBatch batch;
  Fill(&batch, values, 0, 3);
  ASSERT_TRUE(store.Append(batch).ok());

  std::vector<int64_t> more = {20, 21};
  BitVector validity = BitVector::CreateWithSize(2);
  validity.set(0);
  batch.Reset();
  batch.AddColumn(
      ColumnView::Reference(StorageType{Int64{}}, more.data(), &validity));
  batch.Compose(RowSelection::Range(0), 2);
  batch.SetCardinality(2);
  ASSERT_TRUE(store.Append(batch).ok());

  RowBatch out;
  ASSERT_EQ(store.View(&out, 0, 5), 5u);
  EXPECT_THAT(test::ReadNullableColumn<int64_t>(out, 0),
              ElementsAre(10, 11, 12, 20, std::nullopt));
}

TEST(RowStoreTest, RejectsABatchBeforeMutatingAnyColumn) {
  std::vector<int64_t> ints = {1, 2};
  std::vector<double> doubles = {1.0, 2.0};
  RowStore store;
  RowBatch batch;
  batch.AddColumn(ColumnView::Reference(StorageType{Int64{}}, ints.data()));
  batch.AddColumn(ColumnView::Reference(StorageType{Int64{}}, ints.data()));
  batch.SetCardinality(1);
  ASSERT_TRUE(store.Append(batch).ok());

  BitVector validity = BitVector::CreateWithSize(2);
  validity.set(1);
  batch.Reset();
  batch.AddColumn(
      ColumnView::Reference(StorageType{Int64{}}, ints.data(), &validity));
  batch.AddColumn(ColumnView::Reference(StorageType{Double{}}, doubles.data()));
  batch.Compose(RowSelection::Range(1), 1);
  batch.SetCardinality(1);
  EXPECT_FALSE(store.Append(batch).ok());

  RowBatch out;
  ASSERT_EQ(store.View(&out, 0, 1), 1u);
  EXPECT_EQ(out.column(0).validity(), nullptr);
  EXPECT_EQ(store.size(), 1u);
}

TEST(RowStoreTest, AZeroColumnBatchFixesTheSchema) {
  RowStore store;
  RowBatch empty_schema;
  empty_schema.SetCardinality(2);
  ASSERT_TRUE(store.Append(empty_schema).ok());

  std::vector<int64_t> values = {1};
  RowBatch with_column;
  Fill(&with_column, values, 0, 1);
  EXPECT_FALSE(store.Append(with_column).ok());
  EXPECT_EQ(store.size(), 2u);
  EXPECT_EQ(store.column_count(), 0u);
}

TEST(RowStoreTest, ViewingAnEmptySuffixReturnsAnEmptyBatch) {
  std::vector<int64_t> values(RowStore::kChunkRows);
  RowStore store;
  RowBatch batch;
  Fill(&batch, values, 0, RowStore::kChunkRows);
  ASSERT_TRUE(store.Append(batch).ok());

  RowBatch out;
  EXPECT_EQ(store.View(&out, store.size(), 0), 0u);
  EXPECT_EQ(out.size(), 0u);
}

// An Id column has no storage: its value is the row it sits at, so storing one
// means materialising those rows.
TEST(RowStoreTest, AnIdColumnBecomesTheRowsItStoodFor) {
  RowStore store;
  RowBatch batch;
  batch.AddColumn(ColumnView::Reference(StorageType{Id{}}, nullptr, nullptr));
  batch.Compose(RowSelection::Range(7), 3);
  batch.SetCardinality(3);
  ASSERT_TRUE(store.Append(batch).ok());

  RowBatch out;
  store.View(&out, 0, 3);
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
  wider.AddColumn(ColumnView::Reference(StorageType{Int64{}}, values.data()));
  wider.AddColumn(ColumnView::Reference(StorageType{Int64{}}, values.data()));
  wider.Compose(RowSelection::Range(0), 2);
  wider.SetCardinality(2);
  EXPECT_FALSE(store.Append(wider).ok());
}

// Rows already appended are never copied again: each chunk is allocated once
// and stays where it is, so the run a given row sits in keeps its address
// however many batches arrive after it.
TEST(RowStoreTest, FillingAChunkAtATimeDoesNotRecopyTheColumn) {
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

// A run never spans two chunks, so a caller asking for more than the rest of
// one gets the rest of it and comes back for the next.
TEST(RowStoreTest, ARunStopsAtTheEndOfAChunk) {
  std::vector<int64_t> values(RowStore::kChunkRows);
  RowStore store;
  RowBatch batch;
  Fill(&batch, values, 0, RowStore::kChunkRows);
  ASSERT_TRUE(store.Append(batch).ok());
  ASSERT_TRUE(store.Append(batch).ok());

  RowBatch out;
  EXPECT_EQ(store.View(&out, 1, store.size() - 1), RowStore::kChunkRows - 1);
  EXPECT_EQ(store.View(&out, RowStore::kChunkRows,
                       store.size() - RowStore::kChunkRows),
            RowStore::kChunkRows);
}

// A batch which does not divide into the chunk size straddles a boundary, so
// the copy has to split. Every batch size lands differently against it.
TEST(RowStoreTest, BatchesWhichStraddleAChunkBoundary) {
  const uint32_t kRows = RowStore::kChunkRows * 2 + 37;
  std::vector<int64_t> values(RowStore::kChunkRows);
  for (uint32_t i = 0; i < values.size(); ++i) {
    values[i] = 1000 + i;
  }
  for (uint32_t batch_rows : {1u, 7u, 700u, RowStore::kChunkRows}) {
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

// A gather can name rows from any chunk, which no single view can span.
TEST(RowStoreTest, GathersRowsFromSeveralChunks) {
  const uint32_t kRows = RowStore::kChunkRows * 2 + 5;
  std::vector<int64_t> values(RowStore::kChunkRows);
  for (uint32_t i = 0; i < values.size(); ++i) {
    values[i] = static_cast<int64_t>(i);
  }
  RowStore store;
  RowBatch batch;
  std::vector<int64_t> all;
  for (uint32_t done = 0; done < kRows;) {
    uint32_t n = std::min(RowStore::kChunkRows, kRows - done);
    Fill(&batch, values, 0, n);
    ASSERT_TRUE(store.Append(batch).ok());
    all.insert(all.end(), values.begin(), values.begin() + n);
    done += n;
  }
  std::vector<uint32_t> order = {kRows - 1, 0, RowStore::kChunkRows,
                                 RowStore::kChunkRows - 1, 3};
  RowBatch out;
  store.View(&out,
             Span<const uint32_t>(order.data(), order.data() + order.size()));
  std::vector<int64_t> expected;
  for (uint32_t row : order) {
    expected.push_back(all[row]);
  }
  EXPECT_EQ(test::ReadColumn<int64_t>(out, 0), expected);
}

TEST(RowStoreTest, KeepsAColumnWhoseTypeIsPerRow) {
  StringPool pool;
  std::vector<Variant> values = {Variant::Int64(7), Variant::Null(),
                                 Variant::Double(1.5),
                                 Variant::String(pool.InternString("hi"))};
  RowBatch batch;
  batch.AddColumn(ColumnView::Variants(values.data()));
  batch.Compose(RowSelection::Range(0), 4);
  batch.SetCardinality(4);

  RowStore store;
  ASSERT_TRUE(store.Append(batch).ok());
  ASSERT_TRUE(store.is_variant(0));

  RowBatch out;
  ASSERT_EQ(store.View(&out, 0, 4), 4u);
  const auto* kept = static_cast<const Variant*>(out.column(0).data());
  EXPECT_EQ(kept[0].AsInt64(), 7);
  EXPECT_EQ(kept[1].type, Variant::Type::kNull);
  EXPECT_EQ(kept[2].AsDouble(), 1.5);
  EXPECT_EQ(pool.Get(kept[3].AsString()).ToStdString(), "hi");
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
