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

#include "src/trace_processor/core/exec/key_encoder.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/bit_vector.h"

// Laying out keys: a full batch at a time, as an operator keyed by them does
// with its input, and a row at a time, as when a small input is run over and
// over.

namespace perfetto::trace_processor::core::exec {
namespace {

constexpr uint32_t kRows = kMaxBatchRows;

struct Int64s {
  Int64s() : validity(BitVector::CreateWithSize(kRows)) {
    for (uint32_t i = 0; i < kRows; ++i) {
      values.push_back(int64_t{i * 7919 % 1000});
      // One row in eight is null, unpredictably.
      if ((i * 2654435761u) >> 29) {
        validity.set(i);
      }
    }
  }
  std::vector<int64_t> values;
  BitVector validity;
};

void Run(benchmark::State& state, const ColumnView& column, uint32_t rows) {
  RowBatch batch;
  batch.AddBorrowedColumn(column);
  batch.SetRowCount(rows);
  std::vector<uint32_t> columns = {0};
  KeyEncoder encoder;
  for (auto _ : state) {
    benchmark::DoNotOptimize(encoder.Encode(batch, columns));
    benchmark::DoNotOptimize(encoder.Key(rows - 1).data());
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * rows);
}

void BM_KeyEncoderInt64(benchmark::State& state) {
  Int64s c;
  Run(state, ColumnView::Reference(StorageType{Int64{}}, c.values.data()),
      kRows);
}
BENCHMARK(BM_KeyEncoderInt64);

void BM_KeyEncoderInt64Nulls(benchmark::State& state) {
  Int64s c;
  Run(state,
      ColumnView::Reference(StorageType{Int64{}}, c.values.data(), &c.validity),
      kRows);
}
BENCHMARK(BM_KeyEncoderInt64Nulls);

void BM_KeyEncoderInt64OneRow(benchmark::State& state) {
  Int64s c;
  Run(state, ColumnView::Reference(StorageType{Int64{}}, c.values.data()), 1);
}
BENCHMARK(BM_KeyEncoderInt64OneRow);

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
