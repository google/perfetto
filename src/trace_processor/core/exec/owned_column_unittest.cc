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

#include "src/trace_processor/core/exec/owned_column.h"

#include <cstdint>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/span.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using testing::ElementsAre;

template <typename T>
std::vector<T> Read(const ColumnView& column, uint32_t count) {
  const auto* data = static_cast<const T*>(column.data());
  std::vector<T> out;
  for (uint32_t i = 0; i < count; ++i) {
    out.push_back(data[column.selection().GetIndex(i)]);
  }
  return out;
}

TEST(OwnedColumnTest, CopiesTheRunARangePicksOut) {
  std::vector<int64_t> values = {10, 11, 12, 13, 14};
  ColumnView view = ColumnView::Reference(StorageType{Int64{}}, values.data());
  view.selection();

  RowBatch batch;
  batch.AddColumn(view);
  batch.Compose(RowSelection::Range(2), 3);

  OwnedColumn owned;
  owned.Fill(batch.column(0), 3);
  EXPECT_THAT(Read<int64_t>(owned.View(), 3), ElementsAre(12, 13, 14));
}

TEST(OwnedColumnTest, GathersTheRowsAnIndexSelectionPicksOut) {
  std::vector<int64_t> values = {10, 11, 12, 13, 14};
  std::vector<uint32_t> rows = {4, 1, 3};
  ColumnView view = ColumnView::Reference(StorageType{Int64{}}, values.data());
  view.SetBorrowedRows(Span<const uint32_t>(rows.data(), rows.data() + 3));

  OwnedColumn owned;
  owned.Fill(view, 3);
  EXPECT_THAT(Read<int64_t>(owned.View(), 3), ElementsAre(14, 11, 13));
}

// An Id column has no storage: its value is the row it sits at, so a copy of
// one has to write those rows down.
TEST(OwnedColumnTest, AnIdColumnBecomesTheRowsItStoodFor) {
  ColumnView view = ColumnView::Reference(StorageType{Id{}}, nullptr, nullptr);
  RowBatch batch;
  batch.AddColumn(view);
  batch.Compose(RowSelection::Range(7), 3);

  OwnedColumn owned;
  owned.Fill(batch.column(0), 3);
  EXPECT_TRUE(owned.View().type().Is<Uint32>());
  EXPECT_THAT(Read<uint32_t>(owned.View(), 3), ElementsAre(7u, 8u, 9u));
}

TEST(OwnedColumnTest, KeepsWhichRowsHeldNothing) {
  std::vector<int64_t> values = {10, 11, 12, 13};
  BitVector validity = BitVector::CreateWithSize(4);
  validity.set(0);
  validity.set(3);
  ColumnView view =
      ColumnView::Reference(StorageType{Int64{}}, values.data(), &validity);

  OwnedColumn owned;
  owned.Fill(view, 4);
  const BitVector* copied = owned.View().validity();
  ASSERT_NE(copied, nullptr);
  EXPECT_TRUE(copied->is_set(0));
  EXPECT_FALSE(copied->is_set(1));
  EXPECT_FALSE(copied->is_set(2));
  EXPECT_TRUE(copied->is_set(3));
}

// The whole point: a source which refills its buffers leaves a view pointing
// at whatever it wrote next. A copy does not care.
TEST(OwnedColumnTest, SurvivesTheStorageItWasReadFromMovingOn) {
  std::vector<int64_t> buffer = {1, 2, 3};
  ColumnView view = ColumnView::Reference(StorageType{Int64{}}, buffer.data());

  OwnedColumn owned;
  owned.Fill(view, 3);
  buffer = {99, 99, 99};
  EXPECT_THAT(Read<int64_t>(owned.View(), 3), ElementsAre(1, 2, 3));
}

TEST(OwnedColumnTest, CopiesEveryTypeABatchCarries) {
  std::vector<double> doubles = {1.5, 2.5};
  ColumnView double_view =
      ColumnView::Reference(StorageType{Double{}}, doubles.data());
  OwnedColumn owned_double;
  owned_double.Fill(double_view, 2);
  EXPECT_THAT(Read<double>(owned_double.View(), 2), ElementsAre(1.5, 2.5));

  std::vector<uint32_t> uints = {7, 8};
  ColumnView uint_view =
      ColumnView::Reference(StorageType{Uint32{}}, uints.data());
  OwnedColumn owned_uint;
  owned_uint.Fill(uint_view, 2);
  EXPECT_THAT(Read<uint32_t>(owned_uint.View(), 2), ElementsAre(7u, 8u));
}

// A batch that owns its values is also a batch whose columns start dense from
// row zero, which is what lets a computed column be added beside them.
TEST(OwnedColumnTest, AnOwnedColumnIsDenseFromRowZero) {
  std::vector<int64_t> values = {10, 11, 12, 13, 14};
  ColumnView view = ColumnView::Reference(StorageType{Int64{}}, values.data());

  RowBatch source;
  source.AddColumn(view);
  source.Compose(RowSelection::Range(3), 2);

  RowBatch held;
  held.AddOwnedColumn(source.column(0), 2);
  held.SetCardinality(2);
  EXPECT_TRUE(held.column(0).selection().is_range());
  EXPECT_EQ(held.column(0).selection().offset(), 0u);
  EXPECT_THAT(Read<int64_t>(held.column(0), 2), ElementsAre(13, 14));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
