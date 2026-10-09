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
#include <numeric>
#include <random>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/hash_aggregate.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/bit_vector.h"

// GROUP BY one Int64 key computing one to four aggregates, as queries mostly
// compute one to three: COUNT(*), then SUMs over a column with no nulls, one
// with a quarter of its rows null, and another with none. Rows are shuffled,
// so keys don't recur in one fixed order, which would let state laid out by
// group stream through the hardware prefetcher.

namespace perfetto::trace_processor::core::exec {
namespace {

void Run(benchmark::State& state,
         uint32_t distinct,
         uint32_t batch_count,
         uint32_t aggregates) {
  uint32_t rows = batch_count * kMaxBatchRows;
  std::vector<uint32_t> order(rows);
  std::iota(order.begin(), order.end(), 0);
  std::shuffle(order.begin(), order.end(), std::mt19937(1));
  // The key, then the values summed.
  std::vector<int64_t> columns[4];
  BitVector quarter = BitVector::CreateWithSize(rows);
  for (uint32_t i = 0; i < rows; ++i) {
    uint32_t hash = order[i] * 2654435761u;
    columns[0].push_back(hash % distinct);
    columns[1].push_back(hash >> 24);
    columns[2].push_back(hash >> 20);
    columns[3].push_back(static_cast<int64_t>(hash >> 8) - (1 << 23));
    quarter.change(i, hash % 4 != 0);
  }
  std::vector<RowBatch> batches;
  for (uint32_t at = 0; at < rows; at += kMaxBatchRows) {
    RowBatch& batch = batches.emplace_back();
    for (uint32_t c = 0; c < 4; ++c) {
      batch.AddBorrowedColumn(
          ColumnView::Reference(StorageType{Int64{}}, columns[c].data(),
                                c == 2 ? &quarter : nullptr, at));
    }
    batch.SetRowCount(kMaxBatchRows);
  }
  using F = AggregateCall::Function;
  std::vector<AggregateCall> calls = {
      {F::kCountStar, 0}, {F::kSum, 1}, {F::kSum, 2}, {F::kSum, 3}};
  calls.resize(aggregates);
  HashAggregate op(HashAggregateSpec{{0}, calls});
  Context context;
  std::unique_ptr<OperatorState> op_state = op.MakeState(context);
  RowBatch out;
  for (auto _ : state) {
    for (const RowBatch& batch : batches) {
      op.Execute(batch, out, *op_state);
    }
    while (op.Finish(out, *op_state) == OpResult::kHaveMoreOutput) {
      benchmark::DoNotOptimize(out.size());
    }
    op_state->Reset();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * rows);
}

// Arguments: how many distinct keys are spread through 16 batches, and how
// many aggregates.
void BM_HashAggregate(benchmark::State& state) {
  Run(state, static_cast<uint32_t>(state.range(0)), 16,
      static_cast<uint32_t>(state.range(1)));
}
BENCHMARK(BM_HashAggregate)->ArgsProduct({{8, 512, 8192, 32768}, {1, 3}});

// As BM_HashAggregate over 10M rows, for timing against other engines.
void BM_HashAggregateLarge(benchmark::State& state) {
  Run(state, static_cast<uint32_t>(state.range(0)), 4883,
      static_cast<uint32_t>(state.range(1)));
}
BENCHMARK(BM_HashAggregateLarge)
    ->ArgsProduct({{8, 512, 8192, 1 << 20}, {1, 2, 3, 4}})
    ->Unit(benchmark::kMillisecond);

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
