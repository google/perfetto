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
#include "src/trace_processor/core/exec/row_store.h"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/span.h"

// Retaining borrowed batches of Int64 columns and reading them back in another
// order, as an operator holding its input does: two columns over 16 full
// batches in a scattered order, and over one row, as when a small input is run
// over and over; and eight columns over 4096 batches read back in reverse, as
// a tree operator reverses a large input.

namespace perfetto::trace_processor::core::exec {
namespace {

enum class Order { kScattered, kReversed };

void Run(benchmark::State& state,
         uint32_t rows,
         uint32_t column_count,
         Order order_kind) {
  std::vector<std::vector<int64_t>> columns(column_count);
  std::vector<uint32_t> order;
  for (uint32_t i = 0; i < rows; ++i) {
    for (uint32_t c = 0; c < column_count; ++c) {
      columns[c].push_back(int64_t{i} * (c + 1));
    }
    order.push_back(order_kind == Order::kScattered ? (i * 2654435761u) % rows
                                                    : rows - 1 - i);
  }
  std::vector<RowBatch> batches;
  for (uint32_t at = 0; at < rows; at += kMaxBatchRows) {
    uint32_t count = std::min(kMaxBatchRows, rows - at);
    RowBatch& batch = batches.emplace_back();
    for (const std::vector<int64_t>& column : columns) {
      batch.AddBorrowedColumn(ColumnView::Reference(
          StorageType{Int64{}}, column.data(), nullptr, at));
    }
    batch.SetRowCount(count);
  }
  // Before every batch, which hold its buffers.
  Context context;
  RowStore store;
  RowBatch out;
  for (auto _ : state) {
    for (const RowBatch& batch : batches) {
      benchmark::DoNotOptimize(store.Append(batch, context));
    }
    for (uint32_t at = 0; at < rows; at += kMaxBatchRows) {
      const uint32_t* begin = order.data() + at;
      uint32_t count = std::min(kMaxBatchRows, rows - at);
      store.Gather(&out, Span<const uint32_t>(begin, begin + count), context);
      benchmark::DoNotOptimize(out.size());
    }
    store.Clear();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * rows);
}

void BM_RowStore(benchmark::State& state) {
  Run(state, 16 * kMaxBatchRows, 2, Order::kScattered);
}
BENCHMARK(BM_RowStore);

// Many batches held, as when an operator holds a large input: the cost of a
// gather should not grow with how many batches are held.
void BM_RowStoreManyBatchesReversed(benchmark::State& state) {
  Run(state, 4096 * kMaxBatchRows, 8, Order::kReversed);
}
BENCHMARK(BM_RowStoreManyBatchesReversed);

void BM_RowStoreOneRow(benchmark::State& state) {
  Run(state, 1, 2, Order::kScattered);
}
BENCHMARK(BM_RowStoreOneRow);

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
