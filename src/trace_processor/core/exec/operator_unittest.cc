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

#include <cstdint>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/span.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/core/exec/run_vector.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/util/arena.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using test::ArraySource;
using test::Sequence;
using ::testing::ElementsAre;

// Keeps every second row, standing in for a real operator. The rows it picks
// are scratch for one execution, so they live in the state, not the plan.
class DropOddRows final : public Operator {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }
  void DescribeColumns(StepColumns*) const override {}

  OperatorState* NewState(Context& context) const override {
    return context.Make<State>();
  }

  OpResult Execute(const RowBatch& in,
                   RowBatch& out,
                   OperatorState& state) const override {
    uint32_t* selected = state.Cast<State>().selected;
    uint32_t count = 0;
    for (uint32_t row = 0; row < in.size(); row += 2) {
      selected[count++] = row;
    }
    out.CopyFrom(in);
    out.mutable_selection().Keep(
        base::Span<const uint32_t>(selected, selected + count));
    return OpResult::kNeedMoreInput;
  }

 private:
  struct State : OperatorState {
    uint32_t selected[kMaxBatchRows];
  };
};

// Reads a pipeline a row at a time, the way a consumer would.
std::vector<uint32_t> Drain(const Pipeline& pipeline) {
  std::vector<uint32_t> rows;
  RowCursor cursor(pipeline);
  for (cursor.Open(); !cursor.eof(); cursor.Next()) {
    rows.push_back(cursor.Value<uint32_t>(0));
  }
  return rows;
}

// Returns the same input twice, standing in for an operator whose output does
// not fit in a single batch.
class Twice final : public Operator {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }
  void DescribeColumns(StepColumns*) const override {}

  OperatorState* NewState(Context& context) const override {
    return context.Make<State>();
  }

  OpResult Execute(const RowBatch& in,
                   RowBatch& out,
                   OperatorState& state) const override {
    State& s = state.Cast<State>();
    out.CopyFrom(in);
    s.again = !s.again;
    return s.again ? OpResult::kHaveMoreOutput : OpResult::kNeedMoreInput;
  }

 private:
  struct State : OperatorState {
    bool again = false;
  };
};

TEST(OperatorTest, NarrowingAComposedViewKeepsIt) {
  core::Arena arena;
  ArraySource source(arena, Sequence(17));
  std::vector<Pipeline::Step> ops;
  for (int i = 0; i < 3; ++i) {
    ops.push_back(arena.New<DropOddRows>());
  }
  Pipeline pipeline(arena, source, ops);

  // Keeping every second row three times leaves every eighth. The third
  // operator narrows a selection the batch already composed, in place.
  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 8, 16));
}

// Reads every row of every batch, and reads them all again when reopened.
TEST(SinkTest, ReadsEveryRowOfEveryBatch) {
  core::Arena arena;
  ArraySource source(arena, Sequence(kMaxBatchRows * 2 + 7));
  Pipeline pipeline(arena, source, {});

  for (int run = 0; run < 2; ++run) {
    std::vector<uint32_t> rows = Drain(pipeline);
    ASSERT_EQ(rows.size(), kMaxBatchRows * 2u + 7u);
    EXPECT_EQ(rows.front(), 0u);
    EXPECT_EQ(rows[kMaxBatchRows], kMaxBatchRows);
    EXPECT_EQ(rows.back(), kMaxBatchRows * 2u + 6u);
  }
}

// Columns of one batch can be windows starting in different places, as a
// computed column starts at zero whatever its input's window. Reading either
// has to go through that column's own window.
TEST(SinkTest, ReadsColumnsWhichDoNotShareARowView) {
  std::vector<int64_t> payload = {10, 11, 12, 13};
  std::vector<int64_t> computed = {90, 91};

  class TwoViewSource final : public Source {
   public:
    // A local class cannot have static members. No test checks its type.
    const void* type() const override { return nullptr; }

    TwoViewSource(const std::vector<int64_t>* payload,
                  const std::vector<int64_t>* computed)
        : payload_(payload), computed_(computed) {}

    OperatorState* NewState(Context& context) const override {
      return context.Make<State>();
    }
    void Rewind(OperatorState& state) const override {
      state.Cast<State>().done = false;
    }
    bool GetData(RowBatch& batch_, OperatorState& state) const override {
      State& s = state.Cast<State>();
      if (s.done) {
        return false;
      }
      s.done = true;
      batch_.Reset();
      // The payload is read from half way in; the computed column is a
      // separate array read from the start.
      batch_.AddBorrowedColumn(ColumnView::Reference(
          StorageType{Int64{}}, payload_->data(), nullptr, 2));
      batch_.AddBorrowedColumn(
          ColumnView::Reference(StorageType{Int64{}}, computed_->data()));
      batch_.SetRowCount(2);
      return true;
    }

   private:
    struct State : OperatorState {
      bool done = false;
    };
    const std::vector<int64_t>* payload_;
    const std::vector<int64_t>* computed_;
  };

  TwoViewSource source(&payload, &computed);
  RowCursor cursor(source);
  std::vector<int64_t> read_payload;
  std::vector<int64_t> read_computed;
  for (cursor.Open(); !cursor.eof(); cursor.Next()) {
    read_payload.push_back(cursor.Value<int64_t>(0));
    read_computed.push_back(cursor.Value<int64_t>(1));
  }
  EXPECT_THAT(read_payload, ElementsAre(12, 13));
  EXPECT_THAT(read_computed, ElementsAre(90, 91));
}

// A cursor is at eof until it is opened. Once it reports eof, advancing again
// must still report eof.
TEST(SinkTest, EofStaysEof) {
  core::Arena arena;
  ArraySource source(arena, {1, 2});
  Pipeline pipeline(arena, source, {});
  RowCursor cursor(pipeline);
  EXPECT_TRUE(cursor.eof());
  cursor.Open();
  cursor.Next();
  cursor.Next();
  ASSERT_TRUE(cursor.eof());
  EXPECT_FALSE(cursor.Next());
  EXPECT_TRUE(cursor.eof());
}

// Passes every batch through, then lets `count` rows go after the last one,
// one batch each, numbered from `first`. Stands in for an operator holding
// rows back. A negative `first` makes Finish() fail.
class Trailer final : public Operator {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }
  void DescribeColumns(StepColumns*) const override {}

  Trailer(int64_t first, uint32_t count) : first_(first), count_(count) {}

  OperatorState* NewState(Context& context) const override {
    return context.Make<State>();
  }
  OpResult Execute(const RowBatch& in,
                   RowBatch& out,
                   OperatorState&) const override {
    out.CopyFrom(in);
    return OpResult::kNeedMoreInput;
  }
  OpResult Finish(RowBatch& out, OperatorState& state) const override {
    State& s = state.Cast<State>();
    if (first_ < 0) {
      s.context().Fail(base::ErrStatus("trailer broke"));
      return OpResult::kError;
    }
    out.Reset();
    if (s.let_go == count_) {
      return OpResult::kNeedMoreInput;
    }
    out.AddBorrowedColumn(ColumnView::Sequence(nullptr, 0));
    test::Window(&out, static_cast<uint32_t>(first_) + s.let_go, 1);
    return ++s.let_go == count_ ? OpResult::kNeedMoreInput
                                : OpResult::kHaveMoreOutput;
  }

 private:
  struct State : OperatorState {
    uint32_t let_go{};
  };

  int64_t first_;
  uint32_t count_;
};

// Passes every batch through, holding the rows it has seen for the run in
// a vector `seen` points at.
class Recording final : public Operator {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }
  void DescribeColumns(StepColumns*) const override {}

  explicit Recording(const RunVector<uint32_t>** seen) : seen_(seen) {}

  OperatorState* NewState(Context& context) const override {
    auto state = context.Make<State>();
    *seen_ = &state->seen;
    return state;
  }
  OpResult Execute(const RowBatch& in,
                   RowBatch& out,
                   OperatorState& state) const override {
    State& s = state.Cast<State>();
    for (uint32_t i = 0; i < in.size(); ++i) {
      s.seen.push_back(s, in.Value<uint32_t>(0, i));
    }
    out.CopyFrom(in);
    return OpResult::kNeedMoreInput;
  }

 private:
  struct State : OperatorState {
    RunVector<uint32_t> seen;
  };

  const RunVector<uint32_t>** seen_;
};

// What a step holds for a run is let go as soon as nothing more comes out of
// the step, before the steps above it are done, and every run starts empty.
TEST(StateTest, AStepsRunEndsWithItsStream) {
  core::Arena arena;
  ArraySource source(arena, {10, 20});
  const RunVector<uint32_t>* seen = nullptr;
  std::vector<Pipeline::Step> ops;
  ops.push_back(arena.New<Recording>(&seen));
  ops.push_back(arena.New<Trailer>(99, 2u));
  Pipeline pipeline(arena, source, ops);
  Context context;
  OperatorState* state = pipeline.MakeState(context);
  for (uint32_t run = 0; run < 2; ++run) {
    if (run) {
      Restart(pipeline, *state);
    }
    EXPECT_TRUE(seen->empty());
    std::vector<uint32_t> rows;
    RowBatch scratch;
    while (RowBatch* batch = pipeline.Next(scratch, *state)) {
      // The first row of each batch.
      uint32_t row = batch->Value<uint32_t>(0, 0);
      rows.push_back(row);
      // Let go by the trailer once everything below it is done.
      EXPECT_EQ(seen->size(), row >= 99 ? 0u : 2u);
    }
    EXPECT_THAT(rows, ElementsAre(0, 99, 100));
    EXPECT_TRUE(seen->empty());
  }
}

// The deepest operator with more output is drained before the one above it is
// called again.
TEST(OperatorTest, FanOutNestsInnermostFirst) {
  core::Arena arena;
  ArraySource source(arena, {10, 20});
  std::vector<Pipeline::Step> ops;
  ops.push_back(arena.New<Twice>());
  ops.push_back(arena.New<Twice>());
  Pipeline pipeline(arena, source, ops);

  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 1, 0, 1, 0, 1, 0, 1));
}

// What an operator lets go at the end is pushed through the operators above
// it, fan-out included.
TEST(FinishTest, RowsLetGoPassThroughTheOperatorsAbove) {
  core::Arena arena;
  ArraySource source(arena, {10, 20});
  std::vector<Pipeline::Step> ops;
  ops.push_back(arena.New<Trailer>(99, 1u));
  ops.push_back(arena.New<Twice>());
  Pipeline pipeline(arena, source, ops);

  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 1, 0, 1, 99, 99));
}

// An operator is only finished once everything below it has been, so the
// rows the lower one lets go reach it as ordinary input first.
TEST(FinishTest, OperatorsAreFinishedBottomUp) {
  core::Arena arena;
  ArraySource source(arena, {10});
  std::vector<Pipeline::Step> ops;
  ops.push_back(arena.New<Trailer>(7, 1u));
  ops.push_back(arena.New<Trailer>(8, 1u));
  Pipeline pipeline(arena, source, ops);

  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 7, 8));
}

// Keeps every second row in place, remembering the batch it was given.
class DropOddRowsInPlace final : public Transform {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }
  void DescribeColumns(StepColumns*) const override {}

  OperatorState* NewState(Context& context) const override {
    return context.Make<State>();
  }

  bool Process(RowBatch& batch, OperatorState& state) const override {
    State& s = state.Cast<State>();
    seen = &batch;
    uint32_t count = 0;
    for (uint32_t row = 0; row < batch.size(); row += 2) {
      s.selected[count++] = row;
    }
    batch.mutable_selection().Keep(
        base::Span<const uint32_t>(s.selected, s.selected + count));
    return true;
  }

  mutable const RowBatch* seen = nullptr;

 private:
  struct State : OperatorState {
    uint32_t selected[kMaxBatchRows];
  };
};

TEST(OperatorTest, TransformChangesTheBatchHandedOut) {
  core::Arena arena;
  ArraySource source(arena, {10, 20, 30, 40, 50});
  auto drop = arena.New<DropOddRowsInPlace>();
  const DropOddRowsInPlace* transform = drop;
  std::vector<Pipeline::Step> steps;
  steps.push_back(drop);
  Pipeline pipeline(arena, source, steps);

  Context context;
  auto state = pipeline.MakeState(context);
  RowBatch scratch;
  pipeline.Rewind(*state);
  RowBatch* batch = pipeline.Next(scratch, *state);
  ASSERT_NE(batch, nullptr);
  EXPECT_EQ(batch->size(), 3u);
  // Nothing was copied between the transform and the caller.
  EXPECT_EQ(transform->seen, batch);
  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 2, 4));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
