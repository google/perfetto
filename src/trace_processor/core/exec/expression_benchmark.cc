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

#include "src/trace_processor/core/exec/expression.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/bit_vector.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using Op = ExprStep::Op;

constexpr uint32_t kBatches = 512;

// Batches of two Int64 and two Double columns, `a` and `b` of each, and
// validity for each with one row in ten null.
struct Columns {
  Columns() {
    for (uint32_t i = 0; i < kMaxBatchRows; ++i) {
      if (i % 10 != 0) {
        valid_a.set(i);
      }
      if (i % 10 != 5) {
        valid_b.set(i);
      }
      ints_a[i] = i * 7919 % 1000;
      ints_b[i] = i % 97 + 1;
      doubles_a[i] = static_cast<double>(ints_a[i]) * 0.5;
      doubles_b[i] = static_cast<double>(ints_b[i]) * 0.25;
    }
  }
  int64_t ints_a[kMaxBatchRows];
  int64_t ints_b[kMaxBatchRows];
  double doubles_a[kMaxBatchRows];
  double doubles_b[kMaxBatchRows];
  BitVector valid_a = BitVector::CreateWithSize(kMaxBatchRows);
  BitVector valid_b = BitVector::CreateWithSize(kMaxBatchRows);
};

// Runs `op` over the two columns of `type`, with nulls if `nulls`, for
// kBatches batches.
void RunBinary(benchmark::State& state,
               Op op,
               ExprType type,
               bool nulls = false) {
  static Columns* columns = new Columns();
  RowBatch batch;
  bool doubles = type == ExprType::kDouble;
  StorageType storage = doubles ? StorageType{Double{}} : StorageType{Int64{}};
  batch.AddBorrowedColumn(ColumnView::Reference(
      storage,
      doubles ? static_cast<const void*>(columns->doubles_a)
              : static_cast<const void*>(columns->ints_a),
      nulls ? &columns->valid_a : nullptr));
  batch.AddBorrowedColumn(ColumnView::Reference(
      storage,
      doubles ? static_cast<const void*>(columns->doubles_b)
              : static_cast<const void*>(columns->ints_b),
      nulls ? &columns->valid_b : nullptr));
  batch.SetRowCount(kMaxBatchRows);

  ExprProgram program;
  for (uint32_t c = 0; c < 2; ++c) {
    ExprStep read;
    read.op = Op::kColumn;
    read.type = type;
    read.a = c;
    program.steps.push_back(read);
  }
  ExprStep step;
  step.op = op;
  step.type = type;
  step.a = 0;
  step.b = 1;
  program.steps.push_back(step);
  std::vector<ExprProgram> programs;
  programs.push_back(std::move(program));
  Compute compute(std::move(programs));

  Context context;
  auto op_state = compute.MakeState(context);
  RowBatch out;
  for (auto _ : state) {
    for (uint32_t i = 0; i < kBatches; ++i) {
      out.CopyFrom(batch);
      benchmark::DoNotOptimize(compute.Process(out, *op_state));
    }
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kBatches *
                          kMaxBatchRows);
}

void BM_ExprIntAdd(benchmark::State& state) {
  RunBinary(state, Op::kAdd, ExprType::kInt64);
}
BENCHMARK(BM_ExprIntAdd);

void BM_ExprIntMul(benchmark::State& state) {
  RunBinary(state, Op::kMul, ExprType::kInt64);
}
BENCHMARK(BM_ExprIntMul);

void BM_ExprIntDiv(benchmark::State& state) {
  RunBinary(state, Op::kDiv, ExprType::kInt64);
}
BENCHMARK(BM_ExprIntDiv);

void BM_ExprIntAddNullable(benchmark::State& state) {
  RunBinary(state, Op::kAdd, ExprType::kInt64, true);
}
BENCHMARK(BM_ExprIntAddNullable);

void BM_ExprIntDivNullable(benchmark::State& state) {
  RunBinary(state, Op::kDiv, ExprType::kInt64, true);
}
BENCHMARK(BM_ExprIntDivNullable);

void BM_ExprDoubleAdd(benchmark::State& state) {
  RunBinary(state, Op::kAdd, ExprType::kDouble);
}
BENCHMARK(BM_ExprDoubleAdd);

void BM_ExprDoubleMul(benchmark::State& state) {
  RunBinary(state, Op::kMul, ExprType::kDouble);
}
BENCHMARK(BM_ExprDoubleMul);

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
