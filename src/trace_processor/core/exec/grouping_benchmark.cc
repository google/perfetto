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
#include "src/trace_processor/core/exec/grouped_sort.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/bit_vector.h"

// Grouping rows by keys of each shape the executor supports, from a few
// distinct keys to nearly every row its own, with keys shuffled through the
// input or clustered in runs as when the input is ordered by them.

namespace perfetto::trace_processor::core::exec {
namespace {

enum Shape : int64_t {
  kInt64 = 0,
  kNullableInt64 = 1,
  kTwoInt64 = 2,
  kString = 3,
  kDouble = 4,
};

// Arguments: the key's shape, how many distinct keys, 1 if they come in runs
// rather than shuffled, and how many full batches of input.
void BM_GroupBy(benchmark::State& state) {
  auto shape = static_cast<Shape>(state.range(0));
  auto distinct = static_cast<uint32_t>(state.range(1));
  bool clustered = state.range(2) != 0;
  auto rows = static_cast<uint32_t>(state.range(3)) * kMaxBatchRows;

  std::vector<int64_t> ints;
  std::vector<int64_t> seconds;
  std::vector<double> doubles;
  std::vector<StringPool::Id> strings;
  BitVector valid = BitVector::CreateWithSize(rows);
  // Shuffled rows, so keys don't recur in one fixed order, which would let
  // state laid out by group stream through the hardware prefetcher.
  std::vector<uint32_t> order(rows);
  std::iota(order.begin(), order.end(), 0);
  std::shuffle(order.begin(), order.end(), std::mt19937(1));
  for (uint32_t i = 0; i < rows; ++i) {
    uint32_t hash = order[i] * 2654435761u;
    uint32_t key = clustered
                       ? static_cast<uint32_t>(uint64_t{i} * distinct / rows)
                       : hash % distinct;
    // Spread keys out, as ids and timestamps are, rather than 0..distinct.
    ints.push_back(int64_t{key} * 1000003);
    seconds.push_back(int64_t{key} % 7);
    doubles.push_back(key * 0.5);
    strings.push_back(StringPool::Id::Raw(key + 1));
    valid.change(i, hash % 8 != 0);
  }
  std::vector<RowBatch> batches;
  std::vector<uint32_t> keys;
  for (uint32_t at = 0; at < rows; at += kMaxBatchRows) {
    RowBatch& batch = batches.emplace_back();
    switch (shape) {
      case kInt64:
        batch.AddBorrowedColumn(ColumnView::Reference(
            StorageType{Int64{}}, ints.data(), nullptr, at));
        break;
      case kNullableInt64:
        batch.AddBorrowedColumn(ColumnView::Reference(StorageType{Int64{}},
                                                      ints.data(), &valid, at));
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
      case kDouble:
        batch.AddBorrowedColumn(ColumnView::Reference(
            StorageType{Double{}}, doubles.data(), nullptr, at));
        break;
    }
    batch.SetRowCount(kMaxBatchRows);
  }
  keys = shape == kTwoInt64 ? std::vector<uint32_t>{0, 1}
                            : std::vector<uint32_t>{0};

  GroupedSort group_by(GroupedSortSpec{keys, {}});
  Context context;
  std::unique_ptr<OperatorState> op_state = group_by.MakeState(context);
  RowBatch out;
  for (auto _ : state) {
    for (const RowBatch& batch : batches) {
      group_by.Execute(batch, out, *op_state);
    }
    while (group_by.Finish(out, *op_state) == OpResult::kHaveMoreOutput) {
      benchmark::DoNotOptimize(out.size());
    }
    op_state->Reset();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * rows);
}
BENCHMARK(BM_GroupBy)
    // One Int64 key, from few groups to every row its own.
    ->Args({kInt64, 8, 0, 16})
    ->Args({kInt64, 512, 0, 16})
    ->Args({kInt64, 32768, 0, 16})
    ->Args({kInt64, 512, 1, 16})
    // Mostly distinct over a larger input, where the table outgrows caches.
    ->Args({kInt64, 1 << 18, 0, 128})
    // Other shapes, at a moderate number of groups.
    ->Args({kNullableInt64, 512, 0, 16})
    ->Args({kTwoInt64, 512, 0, 16})
    ->Args({kString, 512, 0, 16})
    ->Args({kDouble, 512, 0, 16});

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
