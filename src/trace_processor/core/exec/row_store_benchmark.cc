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

#include "src/trace_processor/core/exec/row_store.h"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/util/span.h"

// Retaining borrowed batches of two Int64 columns and reading them back in
// another order, as an operator holding its input does: over 16 full batches,
// and over one row, as when a small input is run over and over.

namespace perfetto::trace_processor::core::exec {
namespace {

void Run(benchmark::State& state, uint32_t rows) {
  std::vector<int64_t> a;
  std::vector<int64_t> b;
  std::vector<uint32_t> order;
  for (uint32_t i = 0; i < rows; ++i) {
    a.push_back(i);
    b.push_back(i * 7);
    order.push_back((i * 2654435761u) % rows);
  }
  std::vector<RowBatch> batches;
  for (uint32_t at = 0; at < rows; at += kMaxBatchRows) {
    uint32_t count = std::min(kMaxBatchRows, rows - at);
    RowBatch& batch = batches.emplace_back();
    batch.AddColumn(ColumnView::Reference(StorageType{Int64{}}, a.data()));
    batch.AddColumn(ColumnView::Reference(StorageType{Int64{}}, b.data()));
    batch.Compose(RowSelection::Range(at), count);
    batch.SetCardinality(count);
  }
  RowStore store;
  RowBatch out;
  for (auto _ : state) {
    for (const RowBatch& batch : batches) {
      benchmark::DoNotOptimize(store.Append(batch));
    }
    for (uint32_t at = 0; at < rows; at += kMaxBatchRows) {
      const uint32_t* begin = order.data() + at;
      uint32_t count = std::min(kMaxBatchRows, rows - at);
      store.View(&out, Span<const uint32_t>(begin, begin + count));
      benchmark::DoNotOptimize(out.size());
    }
    store.Clear();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * rows);
}

void BM_RowStore(benchmark::State& state) {
  Run(state, 16 * kMaxBatchRows);
}
BENCHMARK(BM_RowStore);

void BM_RowStoreOneRow(benchmark::State& state) {
  Run(state, 1);
}
BENCHMARK(BM_RowStoreOneRow);

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
