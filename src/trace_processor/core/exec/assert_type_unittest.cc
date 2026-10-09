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
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/exec/variant.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {
using testing::Eq;
using testing::Optional;

using testing::ElementsAre;

// Runs the step over a single batch of variants, in place.
struct Asserted {
  Asserted(std::vector<Variant> cells, AssertTypeTarget type)
      : op(0, type, "a"),
        state(op.MakeState(test::TestContext())),
        values(std::move(cells)) {
    input.AddBorrowedColumn(ColumnView::Variants(values.data()));
    test::Window(&input, 0, static_cast<uint32_t>(values.size()));
  }

  // Converts a fresh batch of the values, as a pipeline hands each one over.
  bool Process() {
    batch.CopyFrom(input);
    return op.Process(batch, *state);
  }
  base::Status status() const { return op.status(*state); }

  template <typename T>
  std::vector<T> Read() {
    return test::ReadColumn<T>(batch, 0);
  }

  AssertType op;
  std::unique_ptr<OperatorState> state;
  std::vector<Variant> values;
  RowBatch input;
  // The batch converted in place.
  RowBatch batch;
};

// Only the rows the batch keeps are read; a null is a row holding nothing.
TEST(AssertTypeTest, TurnsVariantsIntoAFlatColumn) {
  Asserted run({Variant::Int64(7), Variant::Int64(8), Variant::Null(),
                Variant::Int64(9)},
               AssertTypeTarget{Int64{}});
  std::vector<uint32_t> kept = {0, 2, 3};
  run.input.mutable_selection().Keep(kept);
  ASSERT_TRUE(run.Process());

  EXPECT_EQ(run.batch.column(0).kind(), ColumnView::Kind::kFlat);
  EXPECT_TRUE(run.batch.column(0).type().Is<Int64>());
  EXPECT_THAT(test::ReadNullableColumn<int64_t>(run.batch, 0),
              ElementsAre(Optional(7), Eq(std::nullopt), Optional(9)));
}

// Each value the type can't hold, as a variant or in a flat column of another
// type, is reported naming the column and what it held.
TEST(AssertTypeTest, AValueTheTypeCannotHoldIsReported) {
  StringPool pool;
  struct VariantCase {
    Variant value;
    AssertTypeTarget type;
    const char* message;
  };
  for (const VariantCase& c : std::vector<VariantCase>{
           {Variant::String(pool.InternString("no")), AssertTypeTarget{Int64{}},
            "a string"},
           {Variant::Int64(std::numeric_limits<int64_t>::max()),
            AssertTypeTarget{Double{}}, "cannot represent exactly"},
           // Above 2^53 a double keeps only the high bits.
           {Variant::Int64((int64_t{1} << 53) + 1), AssertTypeTarget{Double{}},
            "cannot represent exactly"},
       }) {
    Asserted run({Variant::Int64(7), c.value}, c.type);
    EXPECT_FALSE(run.Process());
    EXPECT_THAT(run.status().message(), testing::HasSubstr("'a'"));
    EXPECT_THAT(run.status().message(), testing::HasSubstr(c.message));
  }

  int64_t rounded = (int64_t{1} << 53) + 1;
  double fraction = 1.5;
  uint32_t narrow = 7;
  struct FlatCase {
    StorageType stored;
    const void* value;
    AssertTypeTarget type;
    const char* message;
  };
  for (const FlatCase& c : std::vector<FlatCase>{
           {StorageType{Int64{}}, &rounded, AssertTypeTarget{Double{}},
            "cannot represent exactly"},
           {StorageType{Double{}}, &fraction, AssertTypeTarget{Int64{}}, "'a'"},
           {StorageType{Uint32{}}, &narrow, AssertTypeTarget{String{}},
            "an integer"},
       }) {
    AssertType op(0, c.type, "a");
    std::unique_ptr<OperatorState> state = op.MakeState(test::TestContext());
    RowBatch batch;
    batch.AddBorrowedColumn(ColumnView::Reference(c.stored, c.value));
    batch.SetRowCount(1);
    EXPECT_FALSE(op.Process(batch, *state));
    EXPECT_THAT(op.status(*state).message(), testing::HasSubstr(c.message));
  }
}

TEST(AssertTypeTest, WideningASelectedFlatColumnRemapsValidity) {
  std::vector<uint32_t> values = {7, 8, 9};
  BitVector validity = BitVector::CreateWithSize(3);
  validity.set(1);
  AssertType op(0, AssertTypeTarget{Int64{}}, "a");
  std::unique_ptr<OperatorState> state = op.MakeState(test::TestContext());
  RowBatch batch;
  batch.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Uint32{}}, values.data(), &validity));
  test::Window(&batch, 1, 2);

  ASSERT_TRUE(op.Process(batch, *state));
  EXPECT_THAT(test::ReadNullableColumn<int64_t>(batch, 0),
              ElementsAre(Optional(8), Eq(std::nullopt)));
  EXPECT_THAT(test::ReadColumn<int64_t>(batch, 0), ElementsAre(8, 0));
}

TEST(AssertTypeTest, AReusedNullSlotIsCleared) {
  Asserted run({Variant::Int64(7)}, AssertTypeTarget{Int64{}});
  ASSERT_TRUE(run.Process());
  EXPECT_THAT(run.Read<int64_t>(), ElementsAre(7));

  run.values[0] = Variant::Null();
  ASSERT_TRUE(run.Process());
  EXPECT_THAT(run.Read<int64_t>(), ElementsAre(0));
  EXPECT_FALSE(run.batch.column(0).validity()->is_set(0));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
