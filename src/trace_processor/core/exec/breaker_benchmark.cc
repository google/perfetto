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
#include "src/trace_processor/core/exec/group_by.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/sort.h"

// Sort and GroupBy run over 16 full batches, as over a large input, and over
// one row, as when a small input is run over and over.

namespace perfetto::trace_processor::core::exec {
namespace {

constexpr uint32_t kRows = 16 * kMaxBatchRows;

// Runs `op` over `rows` rows of `key_columns` Int64 keys with `distinct`
// values spread through them.
void Run(benchmark::State& state,
         const Operator& op,
         uint32_t rows,
         uint32_t distinct,
         uint32_t key_columns = 1) {
  std::vector<int64_t> keys;
  for (uint32_t i = 0; i < rows; ++i) {
    keys.push_back(int64_t{(i * 2654435761u) % distinct});
  }
  std::vector<RowBatch> batches;
  for (uint32_t at = 0; at < rows; at += kMaxBatchRows) {
    uint32_t count = std::min(kMaxBatchRows, rows - at);
    RowBatch& batch = batches.emplace_back();
    for (uint32_t k = 0; k < key_columns; ++k) {
      batch.AddColumn(ColumnView::Reference(StorageType{Int64{}}, keys.data(),
                                            nullptr, at));
    }
    batch.SetRowCount(count);
  }
  std::unique_ptr<OperatorState> op_state = op.MakeState();
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

void BM_SortInt64(benchmark::State& state) {
  Sort sort(SortSpec{{{0, false}}});
  Run(state, sort, kRows, 1u << 30);
}
BENCHMARK(BM_SortInt64);

void BM_SortInt64OneRow(benchmark::State& state) {
  Sort sort(SortSpec{{{0, false}}});
  Run(state, sort, 1, 1);
}
BENCHMARK(BM_SortInt64OneRow);

void BM_GroupByInt64(benchmark::State& state) {
  GroupBy group_by({0});
  Run(state, group_by, kRows, static_cast<uint32_t>(state.range(0)));
}
BENCHMARK(BM_GroupByInt64)->Arg(16)->Arg(kRows);

// Keys too long for std::string to hold inline.
void BM_GroupByThreeKeys(benchmark::State& state) {
  GroupBy group_by({0, 1, 2});
  Run(state, group_by, kRows, kRows, 3);
}
BENCHMARK(BM_GroupByThreeKeys);

void BM_GroupByInt64OneRow(benchmark::State& state) {
  GroupBy group_by({0});
  Run(state, group_by, 1, 1);
}
BENCHMARK(BM_GroupByInt64OneRow);

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
