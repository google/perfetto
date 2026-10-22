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

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/distinct.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

// Drops duplicate rows for each kind of key. The number of distinct rows
// ranges from a few to nearly one per row. Rows are either shuffled or
// clustered in runs, as when the input is sorted.

namespace perfetto::trace_processor::core::exec {
namespace {

enum Shape : int64_t {
  kInt64 = 0,
  kTwoInt64 = 1,
  kString = 2,
};

// Arguments: key kind, number of distinct rows, 1 if rows are clustered in
// runs and 0 if shuffled, and number of full input batches.
void BM_Distinct(benchmark::State& state) {
  auto shape = static_cast<Shape>(state.range(0));
  auto distinct = static_cast<uint32_t>(state.range(1));
  bool clustered = state.range(2) != 0;
  auto rows = static_cast<uint32_t>(state.range(3)) * kMaxBatchRows;

  std::vector<int64_t> ints;
  std::vector<int64_t> seconds;
  std::vector<StringPool::Id> strings;
  // Shuffle the rows so keys do not repeat in a fixed order. A fixed order
  // would let the hardware prefetcher hide the cost of random access.
  std::vector<uint32_t> order(rows);
  std::iota(order.begin(), order.end(), 0);
  std::shuffle(order.begin(), order.end(), std::mt19937(1));
  for (uint32_t i = 0; i < rows; ++i) {
    uint32_t key = clustered
                       ? static_cast<uint32_t>(uint64_t{i} * distinct / rows)
                       : (order[i] * 2654435761u) % distinct;
    // Spread keys out like ids and timestamps, instead of 0..distinct.
    ints.push_back(int64_t{key} * 1000003);
    seconds.push_back(int64_t{key} % 7);
    strings.push_back(StringPool::Id::Raw(key + 1));
  }
  std::vector<RowBatch> batches;
  for (uint32_t at = 0; at < rows; at += kMaxBatchRows) {
    RowBatch& batch = batches.emplace_back();
    switch (shape) {
      case kInt64:
        batch.AddBorrowedColumn(ColumnView::Reference(
            StorageType{Int64{}}, ints.data(), nullptr, at));
        break;
      case kTwoInt64:
        batch.AddBorrowedColumn(ColumnView::Reference(
            StorageType{Int64{}}, ints.data(), nullptr, at));
        batch.AddBorrowedColumn(ColumnView::Reference(
            StorageType{Int64{}}, seconds.data(), nullptr, at));
        break;
      case kString:
        batch.AddBorrowedColumn(ColumnView::Reference(
            StorageType{String{}}, strings.data(), nullptr, at));
        break;
    }
    batch.SetRowCount(kMaxBatchRows);
  }
  std::vector<uint32_t> columns = shape == kTwoInt64
                                      ? std::vector<uint32_t>{0, 1}
                                      : std::vector<uint32_t>{0};

  Distinct op(columns);
  Context context;
  std::unique_ptr<OperatorState> op_state = op.MakeState(context);
  RowBatch out;
  for (auto _ : state) {
    for (const RowBatch& batch : batches) {
      out.CopyFrom(batch);
      op.Process(out, *op_state);
      benchmark::DoNotOptimize(out.size());
    }
    op_state->Reset();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * rows);
}
BENCHMARK(BM_Distinct)
    // One Int64 key, from a few distinct rows to one per row.
    ->Args({kInt64, 8, 0, 16})
    ->Args({kInt64, 512, 0, 16})
    ->Args({kInt64, 32768, 0, 16})
    ->Args({kInt64, 512, 1, 16})
    // Mostly distinct rows over a larger input, so the table exceeds caches.
    ->Args({kInt64, 1 << 18, 0, 128})
    // Other key kinds, with a moderate number of distinct rows.
    ->Args({kTwoInt64, 512, 0, 16})
    ->Args({kString, 512, 0, 16});

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
