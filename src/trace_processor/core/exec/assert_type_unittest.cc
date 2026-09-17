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
#include <limits>
#include <memory>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/exec/variant.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {
using testing::Eq;
using testing::Optional;

using testing::ElementsAre;

// Runs the operator over a single batch of variants.
struct Asserted {
  Asserted(std::vector<Variant> cells, AssertTypeTarget type)
      : op(0, type, "a"), state(op.MakeState()), values(std::move(cells)) {
    batch.AddColumn(ColumnView::Variants(values.data()));
    batch.Compose(RowSelection::Range(0), static_cast<uint32_t>(values.size()));
    batch.SetCardinality(static_cast<uint32_t>(values.size()));
  }

  OpResult Execute() { return op.Execute(batch, out, *state); }
  base::Status status() const { return op.status(*state); }

  template <typename T>
  std::vector<T> Read() {
    return test::ReadColumn<T>(out, 0);
  }

  AssertType op;
  std::unique_ptr<OperatorState> state;
  std::vector<Variant> values;
  RowBatch batch;
  RowBatch out;
};

TEST(AssertTypeTest, TurnsVariantsIntoAFlatColumn) {
  Asserted run({Variant::Int64(7), Variant::Int64(8)},
               AssertTypeTarget{Int64{}});
  ASSERT_EQ(run.Execute(), OpResult::kNeedMoreInput);

  EXPECT_EQ(run.out.column(0).kind(), ColumnView::Kind::kFlat);
  EXPECT_TRUE(run.out.column(0).type().Is<Int64>());
  EXPECT_THAT(run.Read<int64_t>(), ElementsAre(7, 8));
}

TEST(AssertTypeTest, ANullIsARowWhichHoldsNothing) {
  Asserted run({Variant::Int64(7), Variant::Null(), Variant::Int64(9)},
               AssertTypeTarget{Int64{}});
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
               AssertTypeTarget{Int64{}});
  EXPECT_EQ(run.Execute(), OpResult::kError);
  EXPECT_FALSE(run.status().ok());
  EXPECT_THAT(run.status().message(), testing::HasSubstr("'a'"));
  EXPECT_THAT(run.status().message(), testing::HasSubstr("a string"));
}

TEST(AssertTypeTest, AnIntegerWidensToAFloat) {
  Asserted run({Variant::Int64(7), Variant::Double(1.5)},
               AssertTypeTarget{Double{}});
  ASSERT_EQ(run.Execute(), OpResult::kNeedMoreInput);
  EXPECT_THAT(run.Read<double>(), ElementsAre(7.0, 1.5));
}

TEST(AssertTypeTest, AnIntegerBeyondTheFloatRangeIsReported) {
  Asserted run({Variant::Int64(std::numeric_limits<int64_t>::max())},
               AssertTypeTarget{Double{}});
  EXPECT_EQ(run.Execute(), OpResult::kError);
  EXPECT_THAT(run.status().message(),
              testing::HasSubstr("cannot represent exactly"));
}

// Above 2^53 a double keeps only the high bits, so a value with low bits set
// has no exact float even though it is well within range.
TEST(AssertTypeTest, AnIntegerAFloatWouldRoundIsReported) {
  Asserted run({Variant::Int64((int64_t{1} << 53) + 1)},
               AssertTypeTarget{Double{}});
  EXPECT_EQ(run.Execute(), OpResult::kError);
  EXPECT_THAT(run.status().message(),
              testing::HasSubstr("cannot represent exactly"));
}

TEST(AssertTypeTest, KeepsStrings) {
  StringPool pool;
  Asserted run({Variant::String(pool.InternString("hi"))},
               AssertTypeTarget{String{}});
  ASSERT_EQ(run.Execute(), OpResult::kNeedMoreInput);
  EXPECT_EQ(pool.Get(run.Read<StringPool::Id>()[0]).ToStdString(), "hi");
}

// Reading through a selection rather than by position, which is what a batch
// narrowed by an earlier operator looks like.
TEST(AssertTypeTest, FollowsTheRowsTheBatchPicksOut) {
  Asserted run({Variant::Int64(10), Variant::Int64(11), Variant::Int64(12)},
               AssertTypeTarget{Int64{}});
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
  AssertType op(0, AssertTypeTarget{Int64{}}, "a");
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
  AssertType op(0, AssertTypeTarget{Int64{}}, "a");
  std::unique_ptr<OperatorState> state = op.MakeState();
  RowBatch batch;
  batch.AddColumn(ColumnView::Reference(StorageType{Double{}}, values.data()));
  batch.Compose(RowSelection::Range(0), 1);
  batch.SetCardinality(1);

  RowBatch out;
  EXPECT_EQ(op.Execute(batch, out, *state), OpResult::kError);
  EXPECT_FALSE(op.status(*state).ok());
}

TEST(AssertTypeTest, WideningASelectedFlatColumnRemapsValidity) {
  std::vector<uint32_t> values = {7, 8, 9};
  BitVector validity = BitVector::CreateWithSize(3);
  validity.set(1);
  AssertType op(0, AssertTypeTarget{Int64{}}, "a");
  std::unique_ptr<OperatorState> state = op.MakeState();
  RowBatch batch;
  batch.AddColumn(
      ColumnView::Reference(StorageType{Uint32{}}, values.data(), &validity));
  batch.Compose(RowSelection::Range(1), 2);
  batch.SetCardinality(2);

  RowBatch out;
  ASSERT_EQ(op.Execute(batch, out, *state), OpResult::kNeedMoreInput);
  EXPECT_THAT(test::ReadNullableColumn<int64_t>(out, 0),
              ElementsAre(Optional(8), Eq(std::nullopt)));
  EXPECT_THAT(test::ReadColumn<int64_t>(out, 0), ElementsAre(8, 0));
}

TEST(AssertTypeTest, FlatIntegersWidenToDouble) {
  std::vector<int64_t> values = {std::numeric_limits<int64_t>::min(), 42};
  AssertType op(0, AssertTypeTarget{Double{}}, "a");
  std::unique_ptr<OperatorState> state = op.MakeState();
  RowBatch batch;
  batch.AddColumn(ColumnView::Reference(StorageType{Int64{}}, values.data()));
  batch.SetCardinality(2);

  RowBatch out;
  ASSERT_EQ(op.Execute(batch, out, *state), OpResult::kNeedMoreInput);
  EXPECT_THAT(test::ReadColumn<double>(out, 0),
              ElementsAre(static_cast<double>(values[0]), 42.0));
}

TEST(AssertTypeTest, AFlatIntegerAFloatWouldRoundIsReported) {
  std::vector<int64_t> values = {(int64_t{1} << 53) + 1};
  AssertType op(0, AssertTypeTarget{Double{}}, "a");
  std::unique_ptr<OperatorState> state = op.MakeState();
  RowBatch batch;
  batch.AddColumn(ColumnView::Reference(StorageType{Int64{}}, values.data()));
  batch.SetCardinality(1);

  RowBatch out;
  EXPECT_EQ(op.Execute(batch, out, *state), OpResult::kError);
  EXPECT_THAT(op.status(*state).message(),
              testing::HasSubstr("cannot represent exactly"));
}

TEST(AssertTypeTest, WideningANonNullFlatColumnStaysNonNull) {
  std::vector<uint32_t> values = {7, 8};
  AssertType op(0, AssertTypeTarget{Int64{}}, "a");
  std::unique_ptr<OperatorState> state = op.MakeState();
  RowBatch batch;
  batch.AddColumn(ColumnView::Reference(StorageType{Uint32{}}, values.data()));
  batch.SetCardinality(2);

  RowBatch out;
  ASSERT_EQ(op.Execute(batch, out, *state), OpResult::kNeedMoreInput);
  EXPECT_EQ(out.column(0).validity(), nullptr);
  EXPECT_THAT(test::ReadColumn<int64_t>(out, 0), ElementsAre(7, 8));
}

TEST(AssertTypeTest, RewindClearsATypeError) {
  StringPool pool;
  Asserted run({Variant::String(pool.InternString("wrong"))},
               AssertTypeTarget{Int64{}});
  ASSERT_EQ(run.Execute(), OpResult::kError);
  ASSERT_FALSE(run.status().ok());
  run.op.Rewind(*run.state);
  EXPECT_TRUE(run.status().ok());
}

TEST(AssertTypeTest, AReusedNullSlotIsCleared) {
  Asserted run({Variant::Int64(7)}, AssertTypeTarget{Int64{}});
  ASSERT_EQ(run.Execute(), OpResult::kNeedMoreInput);
  EXPECT_THAT(run.Read<int64_t>(), ElementsAre(7));

  run.values[0] = Variant::Null();
  ASSERT_EQ(run.Execute(), OpResult::kNeedMoreInput);
  EXPECT_THAT(run.Read<int64_t>(), ElementsAre(0));
  EXPECT_FALSE(run.out.column(0).validity()->is_set(0));
}

TEST(AssertTypeTest, NarrowIntegerErrorsNameAnInteger) {
  std::vector<uint32_t> values = {7};
  AssertType op(0, AssertTypeTarget{String{}}, "a");
  std::unique_ptr<OperatorState> state = op.MakeState();
  RowBatch batch;
  batch.AddColumn(ColumnView::Reference(StorageType{Uint32{}}, values.data()));
  batch.SetCardinality(1);

  RowBatch out;
  ASSERT_EQ(op.Execute(batch, out, *state), OpResult::kError);
  EXPECT_THAT(op.status(*state).message(), testing::HasSubstr("an integer"));
  EXPECT_THAT(op.status(*state).message(), testing::HasSubstr("a string"));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
