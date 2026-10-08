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

#include "src/trace_processor/core/exec/pipeline.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_cursor.h"

// What a pipeline costs by itself: one-row batches through steps which do
// nothing, so the time is the executor's, not any operator's.

namespace perfetto::trace_processor::core::exec {
namespace {

// Hands out `batches` batches of one row each per run.
class OneRowBatches final : public Source {
 public:
  explicit OneRowBatches(uint32_t batches) : batches_(batches) {}

  std::unique_ptr<OperatorState> MakeState() const override {
    return std::make_unique<State>();
  }
  void Rewind(OperatorState& state) const override {
    state.Cast<State>().next = 0;
  }
  bool GetData(RowBatch& out, OperatorState& state) const override {
    State& s = state.Cast<State>();
    if (s.next == batches_) {
      return false;
    }
    out.Reset();
    out.AddColumn(
        ColumnView::Reference(StorageType{Id{}}, nullptr, nullptr, s.next++));
    out.SetRowCount(1);
    return true;
  }

 private:
  struct State : OperatorState {
    uint32_t next = 0;
  };
  uint32_t batches_;
};

class PassThroughOperator final : public Operator {
 public:
  OpResult Execute(const RowBatch& in,
                   RowBatch& out,
                   OperatorState&) const override {
    out.CopyFrom(in);
    return OpResult::kNeedMoreInput;
  }
};

class PassThroughTransform final : public Transform {
 public:
  bool Process(RowBatch&, OperatorState&) const override { return true; }
};

template <typename Step>
void Run(benchmark::State& state) {
  auto steps_count = static_cast<uint32_t>(state.range(0));
  auto batches = static_cast<uint32_t>(state.range(1));
  OneRowBatches source(batches);
  std::vector<Pipeline::Step> steps;
  for (uint32_t i = 0; i < steps_count; ++i) {
    steps.push_back(std::make_unique<Step>());
  }
  Pipeline pipeline(source, std::move(steps), {});
  RowCursor cursor(pipeline);
  for (auto _ : state) {
    uint64_t rows = 0;
    for (cursor.Open(); !cursor.eof(); cursor.Next()) {
      ++rows;
    }
    benchmark::DoNotOptimize(rows);
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * batches);
}

void BM_PipelineOverheadOperators(benchmark::State& state) {
  Run<PassThroughOperator>(state);
}
BENCHMARK(BM_PipelineOverheadOperators)->ArgsProduct({{0, 1, 4}, {1, 64}});

void BM_PipelineOverheadTransforms(benchmark::State& state) {
  Run<PassThroughTransform>(state);
}
BENCHMARK(BM_PipelineOverheadTransforms)->ArgsProduct({{1, 4}, {1, 64}});

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
