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

#include "src/trace_processor/core/exec/breaker.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/core/exec/run_held.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/sink.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using test::ArraySource;
using test::FailingSource;
using test::Sequence;

// Buffers every value and serves them back to front, which is only possible
// once the whole input is in. A negative value is refused.
class Reverse final : public Breaker {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }
  void DescribeColumns(StepColumns*) const override {}

  explicit Reverse(uint32_t column = 1) : column_(column) {}

 private:
  uint32_t column_;

  struct State : Breaker::State {
    ~State() override;
    Held<std::vector<int64_t>> values{this};
    RunValue<uint32_t> served{this};
  };

  Breaker::State* CreateState(Context& context) const override {
    return context.Make<State>();
  }
  bool Consume(const RowBatch& in, Breaker::State& state) const override {
    State& s = state.Cast<State>();
    for (uint32_t row = 0; row < in.size(); ++row) {
      int64_t value = in.Value<int64_t>(column_, row);
      if (value < 0) {
        s.status = base::ErrStatus("negative value");
        return false;
      }
      s.values.push_back(value);
    }
    return true;
  }
  bool Finalize(Breaker::State& state) const override {
    State& s = state.Cast<State>();
    std::reverse(s.values.begin(), s.values.end());
    return true;
  }
  bool Serve(RowBatch& out, Breaker::State& state) const override {
    State& s = state.Cast<State>();
    auto rows = static_cast<uint32_t>(s.values.size());
    if (s.served == rows) {
      return false;
    }
    uint32_t count = std::min(kMaxBatchRows, rows - s.served);
    out.Reset();
    // In a buffer, as the state can go before the batch does.
    ColumnBuffer buffer = s.context->TakeBuffer(sizeof(int64_t));
    int64_t* values = buffer.chunk().Values<int64_t>();
    std::copy_n(s.values.data() + s.served, count, values);
    out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, values),
                  std::move(buffer));
    out.SetRowCount(count);
    s.served += count;
    return true;
  }
};

Reverse::State::~State() = default;

std::vector<int64_t> DrainValues(const Source& source,
                                 const Pipelines& pipelines) {
  std::vector<int64_t> values;
  RowCursor cursor(source, pipelines);
  for (cursor.Open(); !cursor.eof(); cursor.Next()) {
    values.push_back(cursor.Value<int64_t>(0));
  }
  EXPECT_TRUE(cursor.status().ok());
  return values;
}

// Ends a pipeline of `input`, with no steps, in `breaker`, and returns the
// source which serves what it builds.
const Served* EndIn(Pipelines& pipelines,
                    const Source& input,
                    const Breaker* breaker) {
  uint32_t id = pipelines.Add(
      test::TestArena().New<Pipeline>(test::TestArena(), input,
                                      base::Span<const Pipeline::Step>{}),
      breaker);
  return test::TestArena().New<Served>(id, *breaker);
}

TEST(BreakerTest, ServesOnlyOnceTheWholeInputIsIn) {
  ArraySource source(Sequence(kMaxBatchRows * 2 + 7));
  Pipelines pipelines;
  const Served* reverse =
      EndIn(pipelines, source, test::TestArena().New<Reverse>());
  std::vector<int64_t> values = DrainValues(*reverse, pipelines);
  ASSERT_EQ(values.size(), kMaxBatchRows * 2u + 7u);
  EXPECT_EQ(values.front(), kMaxBatchRows * 2 + 6);
  EXPECT_EQ(values.back(), 0);
}

// The first breaker's pipeline ends before the second's starts, which reads
// all the first serves before serving anything itself.
TEST(BreakerTest, ConsecutiveBreakersEachEndAPipeline) {
  auto expected = Sequence(kMaxBatchRows * 2 + 7);
  ArraySource source(expected);
  Pipelines pipelines;
  const Served* first =
      EndIn(pipelines, source, test::TestArena().New<Reverse>());
  const Served* second =
      EndIn(pipelines, *first, test::TestArena().New<Reverse>(0u));
  RowCursor cursor(*second, pipelines);
  // Restart while a breaker still has buffered output to serve.
  ASSERT_TRUE(cursor.Open());
  for (int run = 0; run < 2; ++run) {
    std::vector<int64_t> values;
    for (cursor.Open(); !cursor.eof(); cursor.Next()) {
      values.push_back(cursor.Value<int64_t>(0));
    }
    EXPECT_TRUE(cursor.status().ok());
    EXPECT_EQ(values, expected);
  }
}

TEST(BreakerTest, AFailedInputFailsTheSourceServingIt) {
  FailingSource source;
  Pipelines pipelines;
  const Served* reverse =
      EndIn(pipelines, source, test::TestArena().New<Reverse>());
  RowCursor cursor(*reverse, pipelines);
  EXPECT_FALSE(cursor.Open());
  EXPECT_EQ(cursor.status().message(), "input broke");
}

// The sum of column 1 of what it consumes, counting in `builds` how many
// times it is finalized.
class Total final : public Sink {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  explicit Total(uint32_t* builds) : builds_(builds) {}

  struct Sum : HeldState {
    ~Sum() override;
    RunValue<int64_t> value{this, 0};
  };

  base::Status ConsumeBatch(const RowBatch& in,
                            OperatorState& state) const override {
    Sum& sum = state.Cast<Sum>();
    for (uint32_t row = 0; row < in.size(); ++row) {
      sum.value += in.Value<int64_t>(1, row);
    }
    return base::OkStatus();
  }
  base::Status FinalizeSink(OperatorState&) const override {
    ++*builds_;
    return base::OkStatus();
  }

 private:
  OperatorState* NewSinkState(Context& context) const override {
    return context.Make<Sum>();
  }

  uint32_t* builds_;
};

Total::Sum::~Sum() = default;

// Replaces column 1 of each batch with sink `total`'s sum.
class WithTotal final : public Transform {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  explicit WithTotal(uint32_t total) : total_(total) {}

  struct State : HeldState {
    State(Context& c, uint32_t total) : context(&c), use(this, total) {}
    ~State() override;
    Context* context;
    PipelineUse use;
    base::Status status;
  };

  void DescribeColumns(StepColumns* out) const override { out->reads = {1}; }
  bool Process(RowBatch& batch, OperatorState& state) const override {
    State& s = state.Cast<State>();
    base::StatusOr<OperatorState*> built = s.use.Built(*s.context);
    if (!built.ok()) {
      s.status = built.status();
      return false;
    }
    ColumnBuffer buffer = s.context->TakeBuffer(sizeof(int64_t));
    int64_t* values = buffer.chunk().Values<int64_t>();
    std::fill_n(values, kMaxBatchRows, *(*built)->Cast<Total::Sum>().value);
    batch.SetColumn(1, ColumnView::Reference(StorageType{Int64{}}, values),
                    std::move(buffer));
    return true;
  }
  base::Status status(const OperatorState& state) const override {
    return state.Cast<const State>().status;
  }

 private:
  OperatorState* NewState(Context& context) const override {
    return context.Make<State>(context, total_);
  }

  uint32_t total_;
};

WithTotal::State::~State() = default;

TEST(SinkTest, BuildsOnceARunWhenFirstRead) {
  uint32_t builds = 0;
  ArraySource summed(Sequence(kMaxBatchRows + 4));
  Pipelines pipelines;
  uint32_t total = pipelines.Add(
      test::TestArena().New<Pipeline>(test::TestArena(), summed,
                                      base::Span<const Pipeline::Step>{}),
      test::TestArena().New<Total>(&builds));
  ArraySource rows(Sequence(kMaxBatchRows * 2));
  std::vector<Pipeline::Step> steps;
  steps.push_back(test::TestArena().New<WithTotal>(total));
  Pipeline reads(test::TestArena(), rows, steps);

  int64_t sum = (kMaxBatchRows + 4) * (kMaxBatchRows + 3) / 2;
  RowCursor cursor(reads, pipelines);
  for (uint32_t run = 1; run <= 2; ++run) {
    uint32_t count = 0;
    for (cursor.Open(); !cursor.eof(); cursor.Next()) {
      ASSERT_EQ(cursor.Value<int64_t>(1), sum);
      ++count;
    }
    EXPECT_EQ(count, kMaxBatchRows * 2);
    // Once a run, however many batches read it.
    EXPECT_EQ(builds, run);
  }

  // A step given no rows never reads it, so it is not built.
  ArraySource none(std::vector<int64_t>{});
  std::vector<Pipeline::Step> empty_steps;
  empty_steps.push_back(test::TestArena().New<WithTotal>(total));
  Pipeline empty(test::TestArena(), none, empty_steps);
  RowCursor empty_cursor(empty, pipelines);
  EXPECT_FALSE(empty_cursor.Open());
  EXPECT_EQ(builds, 2u);
}

// A transform whose state counts how many times its run was ended.
class CountEndRuns final : public Transform {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }
  void DescribeColumns(StepColumns*) const override {}

  explicit CountEndRuns(uint32_t* ends) : ends_(ends) {}

  struct Counter {
    explicit Counter(uint32_t* e) : ends(e) {}
    void EndRun() { ++*ends; }
    uint32_t* ends;
  };

  struct State : HeldState {
    explicit State(uint32_t* ends) : counter(this, ends) {}
    ~State() override;
    Held<Counter> counter;
  };

  bool Process(RowBatch&, OperatorState&) const override { return true; }

 private:
  OperatorState* NewState(Context& context) const override {
    return context.Make<State>(ends_);
  }

  uint32_t* ends_;
};

CountEndRuns::State::~State() = default;

TEST(SinkTest, EndPipelineRunEndsHeldStepsBeforeExhaustion) {
  uint32_t ends = 0;
  ArraySource rows(Sequence(kMaxBatchRows * 2));
  std::vector<Pipeline::Step> steps;
  steps.push_back(test::TestArena().New<CountEndRuns>(&ends));
  Pipelines pipelines;
  uint32_t id = pipelines.Add(
      test::TestArena().New<Pipeline>(test::TestArena(), rows, steps), nullptr);

  Context context;
  context.SetPipelines(&pipelines);

  // Pull one batch (leaving the pipeline mid-stream) and end its run early.
  ASSERT_NE(context.NextBatch(id), nullptr);
  EXPECT_EQ(ends, 0u);
  context.EndPipelineRun(id);
  EXPECT_EQ(ends, 1u);

  // Starting a new run must not end the already-ended run a second time.
  ASSERT_NE(context.NextBatch(id), nullptr);
  EXPECT_EQ(ends, 1u);
  while (context.NextBatch(id) != nullptr) {
  }
  EXPECT_EQ(ends, 2u);
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
