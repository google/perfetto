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

#include "src/trace_processor/core/exec/batch_order.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>
#include <random>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// One full batch of (key, ts) rows: 0 in order, 1 shuffled, 2 ordered by ts
// with the keys shuffled, as a ts-sorted table read with a PER key arrives,
// and only the key sorted.
void BM_BatchOrder(benchmark::State& state) {
  std::vector<int64_t> keys(kMaxBatchRows);
  std::vector<int64_t> ts(kMaxBatchRows);
  // Timestamps as a trace has them: nanoseconds well into the trace, so the
  // rows differ in their low four bytes.
  std::minstd_rand rng(1);
  for (uint32_t i = 0; i < kMaxBatchRows; ++i) {
    keys[i] = i / 64;
    ts[i] = int64_t{1} << 40 | int64_t{i} << 20 |
            static_cast<int64_t>(rng() % 1000);
  }
  if (state.range(0) == 2) {
    for (uint32_t i = kMaxBatchRows - 1; i > 0; --i) {
      std::swap(keys[i], keys[rng() % (i + 1)]);
    }
  }
  if (state.range(0) == 1) {
    for (uint32_t i = kMaxBatchRows - 1; i > 0; --i) {
      auto j = static_cast<uint32_t>(rng() % (i + 1));
      std::swap(keys[i], keys[j]);
      std::swap(ts[i], ts[j]);
    }
  }
  RowBatch in;
  in.AddColumn(ColumnView::Reference(StorageType{Int64{}}, keys.data()));
  in.AddColumn(ColumnView::Reference(StorageType{Int64{}}, ts.data()));
  in.Compose(RowSelection::Range(0), kMaxBatchRows);
  in.SetCardinality(kMaxBatchRows);

  BatchOrder order({0, 1}, state.range(0) == 2 ? 1 : 0);
  std::unique_ptr<OperatorState> run = order.MakeState();
  RowBatch out;
  for (auto _ : state) {
    order.Execute(in, out, *run);
    benchmark::DoNotOptimize(out.column(0).selection().data());
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          kMaxBatchRows);
}

BENCHMARK(BM_BatchOrder)->Arg(0)->Arg(1)->Arg(2);

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
