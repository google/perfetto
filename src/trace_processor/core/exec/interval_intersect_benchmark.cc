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

#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/interval_intersect.h"
#include "src/trace_processor/core/exec/interval_lanes.h"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/filter_value_cast.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/filter.h"
#include "src/trace_processor/core/exec/filter_value.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/span.h"

// Intersecting two inputs of intervals keyed 64 ways, over 100K rows each, and
// over four rows, as when a small input is run over and over.

namespace perfetto::trace_processor::core::exec {
namespace {

// ts, dur and one key, handed out as views of their storage.
class Intervals final : public Source {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }
  const Layout& layout() const override { return layout_; }

  explicit Intervals(uint32_t rows) {
    for (uint32_t i = 0; i < rows; ++i) {
      ts_.push_back(int64_t{i} * 10);
      dur_.push_back(15);
      key_.push_back(int64_t{i % 64});
    }
  }
  std::unique_ptr<OperatorState> MakeState(Context&) const override {
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
      out.AddBorrowedColumn(ColumnView::Reference(
          StorageType{Int64{}}, column->data(), nullptr, s.next));
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
  Layout layout_ = Layout(3, ColumnLayout::Of(StorageType{Int64{}}));
};

CollectedInputSpec Operand(const Intervals& source) {
  CollectedInputSpec operand;
  operand.source = &source;
  operand.lanes.ts_column = 0;
  operand.lanes.dur_column = 1;
  operand.lanes.key_columns = {2};
  operand.retained_columns = {2};
  return operand;
}

void Run(benchmark::State& state, uint32_t rows) {
  // Before every batch and state, which hold its buffers.
  Context context;
  Intervals a(rows);
  Intervals b(rows);
  IntervalIntersect intersect({Operand(a), Operand(b)});
  auto op_state = intersect.MakeState(context);
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

// Reads every batch of `source`, its filters comparing with `values`.
void Drain(benchmark::State& state,
           const Source& source,
           const FilterValue* values,
           uint32_t rows) {
  // Before every batch and state, which hold its buffers.
  Context context;
  context.SetFilterValues(Span<const FilterValue>(values, values + 1));
  auto run = source.MakeState(context);
  RowBatch scratch;
  for (auto _ : state) {
    source.Rewind(*run);
    uint64_t regions = 0;
    while (RowBatch* out = source.Next(scratch, *run)) {
      regions += out->size();
    }
    benchmark::DoNotOptimize(regions);
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * rows);
}

// A filter keeping one key of the 64, on the key column.
std::unique_ptr<Filter> KeepOneKey(const StringPool* pool) {
  return std::make_unique<Filter>(std::vector<Filter::Condition>{{2, Eq{}, 0}},
                                  pool);
}

// The value KeepOneKey's filter compares with.
FilterValue OneKey() {
  FilterValue value;
  value.value = filter::CastFilterValueResult::Valid(int64_t{5});
  return value;
}

void BM_IntervalIntersect(benchmark::State& state) {
  Run(state, 100000);
}
BENCHMARK(BM_IntervalIntersect);

void BM_IntervalIntersectFourRows(benchmark::State& state) {
  Run(state, 4);
}
BENCHMARK(BM_IntervalIntersectFourRows);

// One key of the 64, kept by testing the regions intersected from every row.
void BM_IntervalIntersectThenFilter(benchmark::State& state) {
  StringPool pool;
  FilterValue key = OneKey();
  Intervals a(100000);
  Intervals b(100000);
  IntervalIntersect intersect({Operand(a), Operand(b)});
  // The region's bounds, then each operand's key.
  std::vector<Pipeline::Step> steps;
  steps.push_back(KeepOneKey(&pool));
  Pipeline pipeline(intersect, std::move(steps));
  Drain(state, pipeline, &key, 100000);
}
BENCHMARK(BM_IntervalIntersectThenFilter);

// The same key, kept by testing each operand's rows before intersecting
// them: what a WHERE on a PER column passed through the intersection runs.
void BM_IntervalIntersectFilteredOperands(benchmark::State& state) {
  StringPool pool;
  FilterValue key = OneKey();
  Intervals a(100000);
  Intervals b(100000);
  auto filtered = [&pool](const Intervals& source) {
    std::vector<Pipeline::Step> steps;
    steps.push_back(KeepOneKey(&pool));
    return std::make_unique<Pipeline>(source, std::move(steps));
  };
  std::unique_ptr<Pipeline> fa = filtered(a);
  std::unique_ptr<Pipeline> fb = filtered(b);
  CollectedInputSpec oa = Operand(a);
  oa.source = fa.get();
  CollectedInputSpec ob = Operand(b);
  ob.source = fb.get();
  IntervalIntersect intersect({oa, ob});
  Drain(state, intersect, &key, 100000);
}
BENCHMARK(BM_IntervalIntersectFilteredOperands);

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
