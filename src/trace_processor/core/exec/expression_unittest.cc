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

#include "src/trace_processor/core/exec/expression.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using testing::ElementsAre;
using testing::HasSubstr;
using Op = ExprStep::Op;

constexpr auto kNull = std::nullopt;

// Builds programs a step at a time; each call returns the step's register.
class Builder {
 public:
  uint32_t Column(uint32_t column) {
    ExprStep step;
    step.op = Op::kColumn;
    step.a = column;
    return Add(step);
  }
  uint32_t Int(int64_t value) {
    ExprStep step;
    step.op = Op::kConstant;
    step.int_value = value;
    return Add(step);
  }
  uint32_t String(StringPool::Id value) {
    ExprStep step;
    step.op = Op::kConstant;
    step.type = ExprType::kString;
    step.string_value = value;
    return Add(step);
  }
  uint32_t Null() {
    ExprStep step;
    step.op = Op::kNull;
    return Add(step);
  }
  uint32_t Unary(Op op, uint32_t a) {
    ExprStep step;
    step.op = op;
    step.a = a;
    return Add(step);
  }
  uint32_t Binary(Op op,
                  uint32_t a,
                  uint32_t b,
                  ExprType operands = ExprType::kInt64) {
    ExprStep step;
    step.op = op;
    step.a = a;
    step.b = b;
    step.operand_type = operands;
    return Add(step);
  }
  ExprProgram Take() { return std::move(program_); }

 private:
  uint32_t Add(const ExprStep& step) {
    program_.steps.push_back(step);
    return static_cast<uint32_t>(program_.steps.size() - 1);
  }
  ExprProgram program_;
};

// Evaluators are mostly checked end to end by diff tests. These cover what
// those can't easily reach: batches already narrowed or windowed, nulls in
// every position, and failures.
//
// A batch of five rows: #0 is an Id, #1 (`n`) holds 10, 20, null, 40, 50,
// #2 (`u`) holds Uint32 1, 0, 3, 0, 5 and #3 (`s`) "b", "a", "c", "a", "d".
class ExpressionTest : public ::testing::Test {
 protected:
  ExpressionTest() {
    for (uint32_t i = 0; i < 5; ++i) {
      if (i != 2) {
        validity_.set(i);
      }
    }
    for (const char* s : {"b", "a", "c", "a", "d"}) {
      strings_.push_back(pool_.InternString(s));
    }
    batch_.AddBorrowedColumn(ColumnView::Reference(StorageType{Id{}}, nullptr));
    batch_.AddBorrowedColumn(
        ColumnView::Reference(StorageType{Int64{}}, numbers_, &validity_));
    batch_.AddBorrowedColumn(
        ColumnView::Reference(StorageType{Uint32{}}, unsigned_, nullptr));
    batch_.AddBorrowedColumn(
        ColumnView::Reference(StorageType{String{}}, strings_.data(), nullptr));
    batch_.SetRowCount(5);
  }

  // The column `programs` add to the batch, at its kept rows.
  std::vector<std::optional<int64_t>> Run(ExprProgram program) {
    std::vector<ExprProgram> programs;
    programs.push_back(std::move(program));
    Compute compute(std::move(programs));
    auto state = compute.MakeState(test::TestContext());
    RowBatch out;
    EXPECT_TRUE(test::ProcessCopy(compute, batch_, out, *state));
    return test::ReadNullableColumn<int64_t>(out, out.column_count() - 1);
  }

  // The values of #1 at the rows a Where keeps.
  std::vector<std::optional<int64_t>> Keep(ExprProgram program) {
    Where where(std::move(program));
    auto state = where.MakeState(test::TestContext());
    RowBatch out;
    EXPECT_TRUE(test::ProcessCopy(where, batch_, out, *state));
    return test::ReadNullableColumn<int64_t>(out, 1);
  }

  int64_t numbers_[5] = {10, 20, 0, 40, 50};
  uint32_t unsigned_[5] = {1, 0, 3, 0, 5};
  BitVector validity_ = BitVector::CreateWithSize(5);
  StringPool pool_;
  std::vector<StringPool::Id> strings_;
  RowBatch batch_;
};

TEST_F(ExpressionTest, ArithmeticPropagatesNulls) {
  Builder b;
  b.Binary(Op::kAdd, b.Binary(Op::kMul, b.Column(1), b.Int(2)), b.Column(2));
  EXPECT_THAT(Run(b.Take()), ElementsAre(21, 40, kNull, 80, 105));
}

TEST_F(ExpressionTest, IdsReadAsTheirRows) {
  Builder b;
  b.Binary(Op::kMul, b.Column(0), b.Int(3));
  EXPECT_THAT(Run(b.Take()), ElementsAre(0, 3, 6, 9, 12));
}

TEST_F(ExpressionTest, DivisionByZeroIsNull) {
  Builder b;
  b.Binary(Op::kDiv, b.Column(1), b.Column(2));
  EXPECT_THAT(Run(b.Take()), ElementsAre(10, kNull, kNull, kNull, 10));

  Builder m;
  m.Binary(Op::kMod, m.Int(7), m.Column(2));
  EXPECT_THAT(Run(m.Take()), ElementsAre(0, kNull, 1, kNull, 2));
}

TEST_F(ExpressionTest, OverflowFails) {
  Builder b;
  b.Binary(Op::kMul, b.Column(1), b.Int(std::numeric_limits<int64_t>::max()));
  std::vector<ExprProgram> programs;
  programs.push_back(b.Take());
  Compute compute(std::move(programs));
  auto state = compute.MakeState(test::TestContext());
  RowBatch out;
  EXPECT_FALSE(test::ProcessCopy(compute, batch_, out, *state));
  EXPECT_THAT(compute.status(*state).message(), HasSubstr("overflow"));
}

// The null row holds a value which would overflow, but has no value to
// overflow.
TEST_F(ExpressionTest, NullRowsDoNotOverflow) {
  numbers_[2] = std::numeric_limits<int64_t>::max();
  Builder b;
  b.Binary(Op::kAdd, b.Column(1), b.Int(1));
  EXPECT_THAT(Run(b.Take()), ElementsAre(11, 21, kNull, 41, 51));
}

TEST_F(ExpressionTest, ThreeValuedLogic) {
  // n > 15 AND NULL: false where n <= 15, null elsewhere.
  Builder a;
  a.Binary(Op::kAnd, a.Binary(Op::kGt, a.Column(1), a.Int(15)), a.Null());
  EXPECT_THAT(Run(a.Take()), ElementsAre(0, kNull, kNull, kNull, kNull));

  // n > 15 OR NULL: true where n > 15, null elsewhere.
  Builder o;
  o.Binary(Op::kOr, o.Binary(Op::kGt, o.Column(1), o.Int(15)), o.Null());
  EXPECT_THAT(Run(o.Take()), ElementsAre(kNull, 1, kNull, 1, 1));

  Builder n;
  n.Unary(Op::kIsNull, n.Column(1));
  EXPECT_THAT(Run(n.Take()), ElementsAre(0, 0, 1, 0, 0));
}

TEST_F(ExpressionTest, StringEquality) {
  Builder b;
  b.Binary(Op::kEq, b.Column(3), b.String(strings_[1]), ExprType::kString);
  EXPECT_THAT(Run(b.Take()), ElementsAre(0, 1, 0, 1, 0));
}

TEST_F(ExpressionTest, WhereDropsNullAndFalse) {
  Builder b;
  b.Binary(Op::kGe, b.Column(1), b.Int(20));
  EXPECT_THAT(Keep(b.Take()), ElementsAre(20, 40, 50));
}

TEST_F(ExpressionTest, NarrowedBatch) {
  // Keep rows 1, 3 and 4 only: a selection which is not a prefix.
  uint32_t rows[] = {1, 3, 4};
  batch_.mutable_selection().KeepRows(Span<const uint32_t>(rows, rows + 3));
  Builder b;
  b.Binary(Op::kSub, b.Column(1), b.Column(0));
  EXPECT_THAT(Run(b.Take()), ElementsAre(19, 37, 46));

  Builder w;
  w.Binary(Op::kNe, w.Column(3), w.String(strings_[1]), ExprType::kString);
  EXPECT_THAT(Keep(w.Take()), ElementsAre(50));
}

TEST_F(ExpressionTest, WindowedBatch) {
  // Rows 2 to 4 of the columns, as a scan's later batch would see them.
  test::Window(&batch_, 2, 3);
  Builder b;
  b.Binary(Op::kAdd, b.Column(1), b.Column(0));
  EXPECT_THAT(Run(b.Take()), ElementsAre(kNull, 43, 54));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
