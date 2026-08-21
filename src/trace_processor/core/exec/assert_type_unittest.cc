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

#include "src/trace_processor/core/exec/assert_type.h"

#include <cstdint>
#include <memory>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/variant.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using testing::ElementsAre;

// Runs the operator over a single batch of variants.
struct Asserted {
  Asserted(std::vector<Variant> cells, StorageType type)
      : op(0, type, "a"), state(op.MakeState()), values(std::move(cells)) {
    batch.AddColumn(ColumnView::Variants(values.data()));
    batch.Compose(RowSelection::Range(0), static_cast<uint32_t>(values.size()));
    batch.SetCardinality(static_cast<uint32_t>(values.size()));
  }

  OpResult Execute() { return op.Execute(batch, out, *state); }
  base::Status status() const { return op.status(*state); }

  template <typename T>
  std::vector<T> Read() {
    const ColumnView& column = out.column(0);
    const auto* data = static_cast<const T*>(column.data());
    std::vector<T> read;
    for (uint32_t i = 0; i < out.size(); ++i) {
      read.push_back(data[column.selection().GetIndex(i)]);
    }
    return read;
  }

  AssertType op;
  std::unique_ptr<OperatorState> state;
  std::vector<Variant> values;
  RowBatch batch;
  RowBatch out;
};

TEST(AssertTypeTest, TurnsVariantsIntoAFlatColumn) {
  Asserted run({Variant::Int64(7), Variant::Int64(8)}, StorageType{Int64{}});
  ASSERT_EQ(run.Execute(), OpResult::kNeedMoreInput);

  EXPECT_EQ(run.out.column(0).kind(), ColumnView::Kind::kFlat);
  EXPECT_TRUE(run.out.column(0).type().Is<Int64>());
  EXPECT_THAT(run.Read<int64_t>(), ElementsAre(7, 8));
}

TEST(AssertTypeTest, ANullIsARowWhichHoldsNothing) {
  Asserted run({Variant::Int64(7), Variant::Null(), Variant::Int64(9)},
               StorageType{Int64{}});
  ASSERT_EQ(run.Execute(), OpResult::kNeedMoreInput);

  const BitVector* validity = run.out.column(0).validity();
  ASSERT_NE(validity, nullptr);
  EXPECT_TRUE(validity->is_set(0));
  EXPECT_FALSE(validity->is_set(1));
  EXPECT_TRUE(validity->is_set(2));
}

TEST(AssertTypeTest, ARowWhichDisagreesIsReported) {
  StringPool pool;
  Asserted run({Variant::Int64(7), Variant::String(pool.InternString("no"))},
               StorageType{Int64{}});
  EXPECT_EQ(run.Execute(), OpResult::kError);
  EXPECT_FALSE(run.status().ok());
  EXPECT_THAT(run.status().message(), testing::HasSubstr("'a'"));
  EXPECT_THAT(run.status().message(), testing::HasSubstr("a string"));
}

TEST(AssertTypeTest, AnIntegerWidensToAFloat) {
  Asserted run({Variant::Int64(7), Variant::Double(1.5)},
               StorageType{Double{}});
  ASSERT_EQ(run.Execute(), OpResult::kNeedMoreInput);
  EXPECT_THAT(run.Read<double>(), ElementsAre(7.0, 1.5));
}

TEST(AssertTypeTest, AnIntegerTooLargeToWidenIsReported) {
  Asserted run({Variant::Int64((int64_t{1} << 53) + 1)}, StorageType{Double{}});
  EXPECT_EQ(run.Execute(), OpResult::kError);
  EXPECT_THAT(run.status().message(), testing::HasSubstr("too large"));
}

TEST(AssertTypeTest, KeepsStrings) {
  StringPool pool;
  Asserted run({Variant::String(pool.InternString("hi"))},
               StorageType{String{}});
  ASSERT_EQ(run.Execute(), OpResult::kNeedMoreInput);
  EXPECT_EQ(pool.Get(run.Read<StringPool::Id>()[0]).ToStdString(), "hi");
}

// Reading through a selection rather than by position, which is what a batch
// narrowed by an earlier operator looks like.
TEST(AssertTypeTest, FollowsTheRowsTheBatchPicksOut) {
  Asserted run({Variant::Int64(10), Variant::Int64(11), Variant::Int64(12)},
               StorageType{Int64{}});
  std::vector<uint32_t> rows = {2, 0};
  run.batch.mutable_column(0).SetBorrowedRows(
      Span<const uint32_t>(rows.data(), rows.data() + 2));
  run.batch.SetCardinality(2);

  ASSERT_EQ(run.Execute(), OpResult::kNeedMoreInput);
  EXPECT_THAT(run.Read<int64_t>(), ElementsAre(12, 10));
}

// A column which is already the right type is left alone.
TEST(AssertTypeTest, AFlatColumnOfTheRightTypePassesThrough) {
  std::vector<int64_t> values = {1, 2};
  AssertType op(0, StorageType{Int64{}}, "a");
  std::unique_ptr<OperatorState> state = op.MakeState();
  RowBatch batch;
  batch.AddColumn(ColumnView::Reference(StorageType{Int64{}}, values.data()));
  batch.Compose(RowSelection::Range(0), 2);
  batch.SetCardinality(2);

  RowBatch out;
  EXPECT_EQ(op.Execute(batch, out, *state), OpResult::kNeedMoreInput);
  EXPECT_EQ(out.column(0).data(), values.data());
}

TEST(AssertTypeTest, AFlatColumnOfTheWrongTypeIsReported) {
  std::vector<double> values = {1.5};
  AssertType op(0, StorageType{Int64{}}, "a");
  std::unique_ptr<OperatorState> state = op.MakeState();
  RowBatch batch;
  batch.AddColumn(ColumnView::Reference(StorageType{Double{}}, values.data()));
  batch.Compose(RowSelection::Range(0), 1);
  batch.SetCardinality(1);

  RowBatch out;
  EXPECT_EQ(op.Execute(batch, out, *state), OpResult::kError);
  EXPECT_FALSE(op.status(*state).ok());
}

// A dataframe's id column is narrower than an integer, so widening it is
// exact.
TEST(AssertTypeTest, ANarrowerIntegerWidens) {
  std::vector<uint32_t> values = {7, 8, 9};
  AssertType op(0, StorageType{Int64{}}, "a");
  std::unique_ptr<OperatorState> state = op.MakeState();
  RowBatch batch;
  batch.AddColumn(ColumnView::Reference(StorageType{Uint32{}}, values.data()));
  batch.Compose(RowSelection::Range(1), 2);
  batch.SetCardinality(2);

  RowBatch out;
  ASSERT_EQ(op.Execute(batch, out, *state), OpResult::kNeedMoreInput);
  ASSERT_TRUE(out.column(0).type().Is<Int64>());
  const auto* data = static_cast<const int64_t*>(out.column(0).data());
  EXPECT_EQ(data[0], 8);
  EXPECT_EQ(data[1], 9);
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
