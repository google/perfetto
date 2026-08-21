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
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/span.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using testing::ElementsAre;

// Points a batch at one int64 column, the way a source would.
void Fill(RowBatch* batch,
          const std::vector<int64_t>& values,
          uint32_t offset,
          uint32_t count) {
  batch->Reset();
  batch->AddColumn(ColumnView::Reference(StorageType{Int64{}}, values.data()));
  batch->Compose(RowSelection::Range(offset), count);
  batch->SetCardinality(count);
}

std::vector<int64_t> ReadInt64(const RowBatch& batch, uint32_t column) {
  const ColumnView& view = batch.column(column);
  const auto* data = static_cast<const int64_t*>(view.data());
  std::vector<int64_t> out;
  for (uint32_t i = 0; i < batch.size(); ++i) {
    out.push_back(data[view.selection().GetIndex(i)]);
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
  EXPECT_THAT(store.Int64Column(0), ElementsAre(10, 11, 12, 13, 14));
}

// The whole reason a store exists: a source is free to write over the buffer
// a batch was pointing at.
TEST(RowStoreTest, SurvivesTheStorageItWasReadFromMovingOn) {
  std::vector<int64_t> buffer = {1, 2, 3};
  RowStore store;
  RowBatch batch;
  Fill(&batch, buffer, 0, 3);
  ASSERT_TRUE(store.Append(batch).ok());

  buffer = {99, 99, 99};
  EXPECT_THAT(store.Int64Column(0), ElementsAre(1, 2, 3));
}

// A column read back whole is dense from zero however the batches which
// filled it were sliced, which is what lets a fold be a walk over an array.
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

  EXPECT_THAT(store.Int64Column(0), ElementsAre(14, 10, 12));
}

TEST(RowStoreTest, HandsBackARunOfItsRows) {
  std::vector<int64_t> values = {10, 11, 12, 13, 14};
  RowStore store;
  RowBatch batch;
  Fill(&batch, values, 0, 5);
  ASSERT_TRUE(store.Append(batch).ok());

  RowBatch out;
  store.View(&out, 2, 3);
  EXPECT_EQ(out.size(), 3u);
  EXPECT_THAT(ReadInt64(out, 0), ElementsAre(12, 13, 14));
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
  EXPECT_THAT(ReadInt64(out, 0), ElementsAre(14, 13, 10));
}

// Viewing twice must not stack the columns up.
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

// An Id column has no storage: its value is the row it sits at, so keeping
// one means writing those rows down.
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

// Filling a store a chunk at a time must cost the column, not a multiple of
// it. Counting how often the buffer moves is how that stays true without the
// test depending on a clock: growing geometrically moves it a handful of
// times, growing by exactly what was asked for moves it once per chunk.
TEST(RowStoreTest, FillingAChunkAtATimeDoesNotRecopyTheColumn) {
  const uint32_t kRows = 200000, kChunk = 2048;
  std::vector<int64_t> values(kChunk);
  RowStore store;
  RowBatch batch;
  const void* data = nullptr;
  uint32_t moves = 0;
  for (uint32_t done = 0; done < kRows; done += kChunk) {
    Fill(&batch, values, 0, kChunk);
    ASSERT_TRUE(store.Append(batch).ok());
    const void* now = store.Int64Column(0).data();
    moves += now != data;
    data = now;
  }
  EXPECT_EQ(store.size(), ((kRows + kChunk - 1) / kChunk) * kChunk);
  EXPECT_LT(moves, 20u) << "the column is being recopied on every chunk";
}

// A batch keeps the values it was handed alive, so it does not dangle when
// the store does.
TEST(RowStoreTest, ABatchOutlivesTheStore) {
  RowBatch out;
  {
    std::vector<int64_t> values = {10, 11, 12};
    RowStore store;
    RowBatch batch;
    Fill(&batch, values, 0, 3);
    ASSERT_TRUE(store.Append(batch).ok());
    store.View(&out, 0, 3);
  }
  EXPECT_THAT(ReadInt64(out, 0), ElementsAre(10, 11, 12));
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

  Span<const Variant> kept = store.VariantColumn(0);
  ASSERT_EQ(kept.size(), 4u);
  EXPECT_EQ(kept[0].AsInt64(), 7);
  EXPECT_EQ(kept[1].type, Variant::Type::kNull);
  EXPECT_EQ(kept[2].AsDouble(), 1.5);
  EXPECT_EQ(pool.Get(kept[3].AsString()).ToStdString(), "hi");
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
