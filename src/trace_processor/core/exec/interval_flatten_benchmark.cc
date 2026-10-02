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

// Flattening intervals about five deep, counting and summing them: over 16
// full batches, as over a large input, and over one row, as when a small input
// is run over and over.

namespace perfetto::trace_processor::core::exec {
namespace {

void Run(benchmark::State& state, uint32_t rows) {
  std::vector<int64_t> ts;
  std::vector<int64_t> dur;
  std::vector<int64_t> value;
  for (uint32_t i = 0; i < rows; ++i) {
    uint32_t hash = i * 2654435761u;
    ts.push_back(int64_t{i} * 10);
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
    batch.Compose(RowSelection::Range(at), count);
    batch.SetCardinality(count);
  }
  IntervalFlattenSpec spec;
  spec.ts_column = 0;
  spec.dur_column = 1;
  spec.aggregates = {{IntervalFlattenSpec::Function::kCount, 0},
                     {IntervalFlattenSpec::Function::kSum, 2}};
  IntervalFlatten op(spec);
  std::unique_ptr<OperatorState> op_state = op.MakeState();
  RowBatch out;
  for (auto _ : state) {
    for (const RowBatch& batch : batches) {
      op.Execute(batch, out, *op_state);
    }
    while (op.Finish(out, *op_state) == OpResult::kHaveMoreOutput) {
      benchmark::DoNotOptimize(out.size());
    }
    op.Rewind(*op_state);
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * rows);
}

void BM_IntervalFlatten(benchmark::State& state) {
  Run(state, 16 * kMaxBatchRows);
}
BENCHMARK(BM_IntervalFlatten);

void BM_IntervalFlattenOneRow(benchmark::State& state) {
  Run(state, 1);
}
BENCHMARK(BM_IntervalFlattenOneRow);

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
