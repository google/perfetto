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

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/interval_flatten.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"

// Flattening intervals about five deep, counting and summing them. Compare
// small and large inputs, including many small groups, to measure the cost of
// yielding output batches and retaining keys across calls.

namespace perfetto::trace_processor::core::exec {
namespace {

void Run(benchmark::State& state, uint32_t rows, uint32_t rows_per_group = 0) {
  std::vector<int64_t> ts;
  std::vector<int64_t> dur;
  std::vector<int64_t> value;
  std::vector<uint32_t> groups;
  for (uint32_t i = 0; i < rows; ++i) {
    uint32_t hash = i * 2654435761u;
    ts.push_back(static_cast<int64_t>(rows_per_group ? i % rows_per_group : i) *
                 10);
    groups.push_back(rows_per_group ? i / rows_per_group : 0);
    dur.push_back(hash >> 26);
    value.push_back(hash >> 24);
  }
  std::vector<RowBatch> batches;
  for (uint32_t at = 0; at < rows; at += kMaxBatchRows) {
    uint32_t count = std::min(kMaxBatchRows, rows - at);
    RowBatch& batch = batches.emplace_back();
    for (const std::vector<int64_t>* column : {&ts, &dur, &value}) {
      batch.AddColumn(
          ColumnView::Reference(StorageType{Int64{}}, column->data()));
    }
    if (rows_per_group) {
      batch.AddColumn(
          ColumnView::Reference(StorageType{Uint32{}}, groups.data()));
    }
    batch.Compose(RowSelection::Range(at), count);
    batch.SetCardinality(count);
  }
  IntervalFlattenSpec spec;
  spec.ts_column = 0;
  spec.dur_column = 1;
  spec.aggregates = {{IntervalFlattenSpec::Function::kCount, 0},
                     {IntervalFlattenSpec::Function::kSum, 2}};
  if (rows_per_group) {
    spec.key_columns = {3};
    spec.group_column = 3;
  }
  IntervalFlatten op(spec);
  std::unique_ptr<OperatorState> op_state = op.MakeState();
  RowBatch out;
  for (auto _ : state) {
    for (const RowBatch& batch : batches) {
      OpResult result;
      do {
        result = op.Execute(batch, out, *op_state);
        benchmark::DoNotOptimize(out.size());
      } while (result == OpResult::kHaveMoreOutput);
    }
    while (op.Finish(out, *op_state) == OpResult::kHaveMoreOutput) {
      benchmark::DoNotOptimize(out.size());
    }
    op_state->Reset();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * rows);
}

void BM_IntervalFlatten(benchmark::State& state) {
  Run(state, static_cast<uint32_t>(state.range(0)) * kMaxBatchRows,
      static_cast<uint32_t>(state.range(1)));
}
BENCHMARK(BM_IntervalFlatten)
    ->Args({16, 0})
    ->Args({512, 0})
    ->Args({16, 1})
    ->Args({16, 128});

void BM_IntervalFlattenOneRow(benchmark::State& state) {
  Run(state, 1);
}
BENCHMARK(BM_IntervalFlattenOneRow);

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
