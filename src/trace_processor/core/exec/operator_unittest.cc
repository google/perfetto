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
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/util/span.h"
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
  std::unique_ptr<OperatorState> MakeState() const override {
    return std::make_unique<State>();
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
    out.Slice(
        RowSelection::Indices(Span<const uint32_t>(selected, selected + count)),
        count);
    return OpResult::kNeedMoreInput;
  }

 private:
  struct State : OperatorState {
    ~State() override;
    uint32_t selected[kMaxBatchRows];
  };
};

DropOddRows::State::~State() = default;

// Drives a plan the way an executor does: it creates the state and owns the
// batch, leaving the plan const throughout.
class Execution {
 public:
  explicit Execution(const Source& source)
      : source_(source), state_(source.MakeState()) {}

  RowBatch* Next() {
    return source_.GetData(batch_, *state_) ? &batch_ : nullptr;
  }
  void Rewind() { source_.Rewind(*state_); }

 private:
  const Source& source_;
  std::unique_ptr<OperatorState> state_;
  RowBatch batch_;
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
  std::unique_ptr<OperatorState> MakeState() const override {
    return std::make_unique<State>();
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
    ~State() override;
    bool again = false;
  };
};

Twice::State::~State() = default;

TEST(OperatorTest, SourceEmitsEveryRow) {
  ArraySource source({10, 20, 30});
  Pipeline pipeline(source, {});

  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 1, 2));
}

TEST(OperatorTest, SourceSplitsIntoBatches) {
  ArraySource source(Sequence(kMaxBatchRows * 2 + 3));
  Pipeline pipeline(source, {});

  std::vector<uint32_t> rows = Drain(pipeline);
  ASSERT_EQ(rows.size(), kMaxBatchRows * 2u + 3u);
  EXPECT_EQ(rows.front(), 0u);
  EXPECT_EQ(rows.back(), kMaxBatchRows * 2u + 2u);
}

TEST(OperatorTest, SourceIsReplayable) {
  ArraySource source({10, 20, 30});
  Pipeline pipeline(source, {});

  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 1, 2));
  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 1, 2));
}

TEST(OperatorTest, OperatorNarrowsTheBatch) {
  ArraySource source({10, 20, 30, 40, 50});
  std::vector<std::unique_ptr<Operator>> ops;
  ops.push_back(std::make_unique<DropOddRows>());
  Pipeline pipeline(source, std::move(ops));

  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 2, 4));
}

TEST(OperatorTest, OperatorsComposeWithinABatch) {
  ArraySource source({0, 1, 2, 3, 4, 5, 6, 7, 8});
  std::vector<std::unique_ptr<Operator>> ops;
  ops.push_back(std::make_unique<DropOddRows>());
  ops.push_back(std::make_unique<DropOddRows>());
  Pipeline pipeline(source, std::move(ops));

  // Keeping every second row twice leaves every fourth.
  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 4, 8));
}

TEST(OperatorTest, NarrowingAComposedViewKeepsIt) {
  ArraySource source(Sequence(17));
  std::vector<std::unique_ptr<Operator>> ops;
  for (int i = 0; i < 3; ++i) {
    ops.push_back(std::make_unique<DropOddRows>());
  }
  Pipeline pipeline(source, std::move(ops));

  // Keeping every second row three times leaves every eighth. The third
  // operator narrows a selection the batch already composed, in place.
  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 8, 16));
}

// The storage a batch composes its selections into belongs to the batch and is
// reused by the next one, so a pipeline stops allocating once it is running.
TEST(OperatorTest, ComposedViewsReuseTheBatchesStorage) {
  ArraySource source(Sequence(kMaxBatchRows * 4));
  std::vector<std::unique_ptr<Operator>> ops;
  ops.push_back(std::make_unique<DropOddRows>());
  ops.push_back(std::make_unique<DropOddRows>());
  Pipeline pipeline(source, std::move(ops));

  Execution run(pipeline);
  RowBatch* batch = run.Next();
  ASSERT_NE(batch, nullptr);
  const uint32_t* block = batch->column(0).selection().data();
  ASSERT_NE(block, nullptr) << "expected a composed view";
  uint32_t batches = 1;
  while ((batch = run.Next()) != nullptr) {
    EXPECT_EQ(batch->column(0).selection().data(), block);
    ++batches;
  }
  EXPECT_EQ(batches, 4u);
}

TEST(SinkTest, ReadsEveryRowOfEveryBatch) {
  ArraySource source(Sequence(kMaxBatchRows * 2 + 7));
  Pipeline pipeline(source, {});

  std::vector<uint32_t> rows = Drain(pipeline);
  ASSERT_EQ(rows.size(), kMaxBatchRows * 2u + 7u);
  EXPECT_EQ(rows.front(), 0u);
  EXPECT_EQ(rows[kMaxBatchRows], kMaxBatchRows);
  EXPECT_EQ(rows.back(), kMaxBatchRows * 2u + 6u);
}

// An operator adding a computed column cannot put it in the index space its
// input arrived in, so it uses its own. Reading either column has to go
// through that column's own selection.
TEST(SinkTest, ReadsColumnsWhichDoNotShareARowView) {
  std::vector<int64_t> payload = {10, 11, 12, 13};
  std::vector<int64_t> computed = {90, 91};

  class TwoViewSource final : public Source {
   public:
    TwoViewSource(const std::vector<int64_t>* payload,
                  const std::vector<int64_t>* computed)
        : payload_(payload), computed_(computed) {}

    std::unique_ptr<OperatorState> MakeState() const override {
      return std::make_unique<State>();
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
      batch_.AddColumn(
          ColumnView::Reference(StorageType{Int64{}}, payload_->data()));
      // The payload is read from half way in; the computed column is a
      // separate array read from the start.
      batch_.Compose(RowSelection::Range(2), 2);
      batch_.AddColumn(
          ColumnView::Reference(StorageType{Int64{}}, computed_->data()));
      batch_.SetCardinality(2);
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

TEST(SinkTest, ReportsEofWithoutOpen) {
  ArraySource source({1, 2, 3});
  Pipeline pipeline(source, {});
  RowCursor cursor(pipeline);

  EXPECT_TRUE(cursor.eof());
}

// A cursor which has reported eof has no batch left to move within, so
// advancing again must keep saying so rather than resurrecting the cursor.
TEST(SinkTest, AdvancingPastEofStaysAtEof) {
  ArraySource source({1, 2});
  Pipeline pipeline(source, {});
  RowCursor cursor(pipeline);
  cursor.Open();
  cursor.Next();
  cursor.Next();
  ASSERT_TRUE(cursor.eof());
  EXPECT_FALSE(cursor.Next());
  EXPECT_TRUE(cursor.eof());
}

TEST(SinkTest, ReopeningRereadsFromTheStart) {
  ArraySource source({4, 5, 6});
  Pipeline pipeline(source, {});

  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 1, 2));
  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 1, 2));
}

// One input batch can produce more than one output batch: the operator says
// so and is called again with the same input.
TEST(OperatorTest, AnOperatorCanFanOut) {
  ArraySource source({10, 20, 30});
  std::vector<std::unique_ptr<Operator>> ops;
  ops.push_back(std::make_unique<Twice>());
  Pipeline pipeline(source, std::move(ops));

  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 1, 2, 0, 1, 2));
}

// The deepest operator with more output is drained before the one above it is
// called again.
TEST(OperatorTest, FanOutNestsInnermostFirst) {
  ArraySource source({10, 20});
  std::vector<std::unique_ptr<Operator>> ops;
  ops.push_back(std::make_unique<Twice>());
  ops.push_back(std::make_unique<Twice>());
  Pipeline pipeline(source, std::move(ops));

  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 1, 0, 1, 0, 1, 0, 1));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
