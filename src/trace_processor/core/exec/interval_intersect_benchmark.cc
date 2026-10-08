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

#include "src/trace_processor/core/exec/interval_intersect.h"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"

// Intersecting two inputs of intervals keyed 64 ways, over 100K rows each, and
// over four rows, as when a small input is run over and over.

namespace perfetto::trace_processor::core::exec {
namespace {

// ts, dur and one key, handed out as views of their storage.
class Intervals final : public Source {
 public:
  explicit Intervals(uint32_t rows) {
    for (uint32_t i = 0; i < rows; ++i) {
      ts_.push_back(int64_t{i} * 10);
      dur_.push_back(15);
      key_.push_back(int64_t{i % 64});
    }
  }
  std::unique_ptr<OperatorState> MakeState() const override {
    return std::make_unique<State>();
  }
  void Rewind(OperatorState& state) const override {
    state.Cast<State>().next = 0;
  }
  bool GetData(RowBatch& out, OperatorState& state) const override {
    State& s = state.Cast<State>();
    auto rows = static_cast<uint32_t>(ts_.size());
    if (s.next == rows) {
      return false;
    }
    uint32_t count = std::min(kMaxBatchRows, rows - s.next);
    out.Reset();
    for (const std::vector<int64_t>* column : {&ts_, &dur_, &key_}) {
      out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, column->data(),
                                          nullptr, s.next));
    }
    out.SetRowCount(count);
    s.next += count;
    return true;
  }

 private:
  struct State : OperatorState {
    uint32_t next = 0;
  };
  std::vector<int64_t> ts_;
  std::vector<int64_t> dur_;
  std::vector<int64_t> key_;
};

IntervalIntersectOperand Operand(const Intervals& source) {
  IntervalIntersectOperand operand;
  operand.source = &source;
  operand.ts_column = 0;
  operand.dur_column = 1;
  operand.key_columns = {2};
  operand.retained_columns = {2};
  return operand;
}

void Run(benchmark::State& state, uint32_t rows) {
  Intervals a(rows);
  Intervals b(rows);
  IntervalIntersect intersect({Operand(a), Operand(b)});
  auto op_state = intersect.MakeState();
  RowBatch out;
  for (auto _ : state) {
    intersect.Rewind(*op_state);
    uint64_t regions = 0;
    while (intersect.GetData(out, *op_state)) {
      regions += out.size();
    }
    benchmark::DoNotOptimize(regions);
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * rows);
}

void BM_IntervalIntersect(benchmark::State& state) {
  Run(state, 100000);
}
BENCHMARK(BM_IntervalIntersect);

void BM_IntervalIntersectFourRows(benchmark::State& state) {
  Run(state, 4);
}
BENCHMARK(BM_IntervalIntersectFourRows);

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
