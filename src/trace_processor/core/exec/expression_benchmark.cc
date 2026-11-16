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
#include <string>
#include <utility>
#include <vector>

#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/containers/string_pool.h"
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
  auto columns = std::make_unique<Columns>();
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
  StringPool pool;
  Compute compute(std::move(programs), &pool);

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

// Column ids in RunProgram's batch.
enum : uint32_t { kIntA, kIntB, kDoubleA, kDoubleB, kStringA, kStringB };

// Strings with many values and some repeats, for string columns, and the pool
// holding them.
struct Strings {
  Strings() {
    for (uint32_t i = 0; i < kMaxBatchRows; ++i) {
      std::string text = "com.example.Class" + std::to_string(i % 500) +
                         (i % 3 == 0 ? "$Stub" : "");
      a[i] = pool.InternString(base::StringView(text));
      b[i] = pool.InternString(
          base::StringView("com.example.Class" + std::to_string(i % 97)));
    }
  }
  StringPool pool;
  StringPool::Id a[kMaxBatchRows];
  StringPool::Id b[kMaxBatchRows];
};

// Runs `program` over batches of the columns above, with nulls if `nulls`.
void RunProgram(benchmark::State& state, ExprProgram program, bool nulls) {
  auto columns = std::make_unique<Columns>();
  auto strings = std::make_unique<Strings>();
  const BitVector* a = nulls ? &columns->valid_a : nullptr;
  const BitVector* b = nulls ? &columns->valid_b : nullptr;
  RowBatch batch;
  batch.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Int64{}}, columns->ints_a, a));
  batch.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Int64{}}, columns->ints_b, b));
  batch.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Double{}}, columns->doubles_a, a));
  batch.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Double{}}, columns->doubles_b, b));
  batch.AddBorrowedColumn(
      ColumnView::Reference(StorageType{String{}}, strings->a, a));
  batch.AddBorrowedColumn(
      ColumnView::Reference(StorageType{String{}}, strings->b, b));
  batch.SetRowCount(kMaxBatchRows);

  std::vector<ExprProgram> programs;
  programs.push_back(std::move(program));
  Compute compute(std::move(programs), &strings->pool);
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

uint32_t Read(ExprProgram* program, uint32_t column, ExprType type) {
  ExprStep step;
  step.op = Op::kColumn;
  step.type = type;
  step.a = column;
  program->steps.push_back(step);
  return static_cast<uint32_t>(program->steps.size() - 1);
}

uint32_t Apply(ExprProgram* program,
               Op op,
               ExprType type,
               uint32_t a,
               uint32_t b = 0,
               uint32_t c = 0) {
  ExprStep step;
  step.op = op;
  step.type = type;
  step.a = a;
  step.b = b;
  step.c = c;
  program->steps.push_back(step);
  return static_cast<uint32_t>(program->steps.size() - 1);
}

void RunCompare(benchmark::State& state,
                Op op,
                ExprType type,
                uint32_t a,
                uint32_t b,
                bool nulls = false) {
  ExprProgram p;
  uint32_t x = Read(&p, a, type);
  uint32_t y = Read(&p, b, type);
  Apply(&p, op, ExprType::kInt64, x, y);
  RunProgram(state, std::move(p), nulls);
}

void BM_ExprIntEq(benchmark::State& state) {
  RunCompare(state, Op::kEq, ExprType::kInt64, kIntA, kIntB);
}
BENCHMARK(BM_ExprIntEq);

void BM_ExprIntNe(benchmark::State& state) {
  RunCompare(state, Op::kNe, ExprType::kInt64, kIntA, kIntB);
}
BENCHMARK(BM_ExprIntNe);

void BM_ExprIntLt(benchmark::State& state) {
  RunCompare(state, Op::kLt, ExprType::kInt64, kIntA, kIntB);
}
BENCHMARK(BM_ExprIntLt);

void BM_ExprIntGt(benchmark::State& state) {
  RunCompare(state, Op::kGt, ExprType::kInt64, kIntA, kIntB);
}
BENCHMARK(BM_ExprIntGt);

void BM_ExprIntLtNullable(benchmark::State& state) {
  RunCompare(state, Op::kLt, ExprType::kInt64, kIntA, kIntB, true);
}
BENCHMARK(BM_ExprIntLtNullable);

void BM_ExprDoubleLt(benchmark::State& state) {
  RunCompare(state, Op::kLt, ExprType::kDouble, kDoubleA, kDoubleB);
}
BENCHMARK(BM_ExprDoubleLt);

void BM_ExprDoubleGe(benchmark::State& state) {
  RunCompare(state, Op::kGe, ExprType::kDouble, kDoubleA, kDoubleB);
}
BENCHMARK(BM_ExprDoubleGe);

void BM_ExprStringEq(benchmark::State& state) {
  RunCompare(state, Op::kEq, ExprType::kString, kStringA, kStringB);
}
BENCHMARK(BM_ExprStringEq);

void BM_ExprStringNe(benchmark::State& state) {
  RunCompare(state, Op::kNe, ExprType::kString, kStringA, kStringB);
}
BENCHMARK(BM_ExprStringNe);

void BM_ExprStringLt(benchmark::State& state) {
  RunCompare(state, Op::kLt, ExprType::kString, kStringA, kStringB);
}
BENCHMARK(BM_ExprStringLt);

void RunGlob(benchmark::State& state, const char* pattern, bool nulls) {
  ExprProgram p;
  uint32_t x = Read(&p, kStringA, ExprType::kString);
  Apply(&p, Op::kGlob, ExprType::kInt64, x, 0);
  p.patterns.push_back(pattern);
  RunProgram(state, std::move(p), nulls);
}

void BM_ExprGlobSuffix(benchmark::State& state) {
  RunGlob(state, "*$Stub", false);
}
BENCHMARK(BM_ExprGlobSuffix);

void BM_ExprGlobSuffixNullable(benchmark::State& state) {
  RunGlob(state, "*$Stub", true);
}
BENCHMARK(BM_ExprGlobSuffixNullable);

// Every string new to the evaluator, so nothing it remembers helps.
void BM_ExprGlobNoRepeats(benchmark::State& state) {
  constexpr uint32_t kRuns = 32;
  StringPool pool;
  std::vector<StringPool::Id> ids;
  for (uint32_t i = 0; i < kMaxBatchRows * kRuns; ++i) {
    std::string text =
        "com.example.Class" + std::to_string(i) + (i % 3 == 0 ? "$Stub" : "");
    ids.push_back(pool.InternString(base::StringView(text)));
  }
  ExprProgram p;
  uint32_t x = Read(&p, 0, ExprType::kString);
  Apply(&p, Op::kGlob, ExprType::kInt64, x, 0);
  p.patterns.push_back("*$Stub");
  std::vector<ExprProgram> programs;
  programs.push_back(std::move(p));
  Compute compute(std::move(programs), &pool);
  Context context;
  for (auto _ : state) {
    auto op_state = compute.MakeState(context);
    for (uint32_t i = 0; i < kRuns; ++i) {
      RowBatch batch;
      batch.AddBorrowedColumn(ColumnView::Reference(
          StorageType{String{}}, ids.data() + i * kMaxBatchRows, nullptr));
      batch.SetRowCount(kMaxBatchRows);
      benchmark::DoNotOptimize(compute.Process(batch, *op_state));
    }
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kRuns *
                          kMaxBatchRows);
}
BENCHMARK(BM_ExprGlobNoRepeats);

void RunNullTest(benchmark::State& state, Op op, bool nulls) {
  ExprProgram p;
  uint32_t x = Read(&p, kIntA, ExprType::kInt64);
  Apply(&p, op, ExprType::kInt64, x);
  RunProgram(state, std::move(p), nulls);
}

void BM_ExprIsNull(benchmark::State& state) {
  RunNullTest(state, Op::kIsNull, true);
}
BENCHMARK(BM_ExprIsNull);

void BM_ExprIsNotNull(benchmark::State& state) {
  RunNullTest(state, Op::kIsNotNull, true);
}
BENCHMARK(BM_ExprIsNotNull);

void BM_ExprIsNullNoNulls(benchmark::State& state) {
  RunNullTest(state, Op::kIsNull, false);
}
BENCHMARK(BM_ExprIsNullNoNulls);

void RunCoalesce(benchmark::State& state,
                 ExprType type,
                 uint32_t a,
                 uint32_t b,
                 bool nulls) {
  ExprProgram p;
  uint32_t x = Read(&p, a, type);
  uint32_t y = Read(&p, b, type);
  Apply(&p, Op::kCoalesce, type, x, y);
  RunProgram(state, std::move(p), nulls);
}

void BM_ExprCoalesceInt(benchmark::State& state) {
  RunCoalesce(state, ExprType::kInt64, kIntA, kIntB, true);
}
BENCHMARK(BM_ExprCoalesceInt);

void BM_ExprCoalesceString(benchmark::State& state) {
  RunCoalesce(state, ExprType::kString, kStringA, kStringB, true);
}
BENCHMARK(BM_ExprCoalesceString);

void BM_ExprCoalesceNoNulls(benchmark::State& state) {
  RunCoalesce(state, ExprType::kInt64, kIntA, kIntB, false);
}
BENCHMARK(BM_ExprCoalesceNoNulls);

void RunIif(benchmark::State& state,
            ExprType type,
            uint32_t a,
            uint32_t b,
            bool nulls) {
  ExprProgram p;
  uint32_t c = Read(&p, kIntA, ExprType::kInt64);
  uint32_t x = Read(&p, a, type);
  uint32_t y = Read(&p, b, type);
  Apply(&p, Op::kIif, type, c, x, y);
  RunProgram(state, std::move(p), nulls);
}

void BM_ExprIifInt(benchmark::State& state) {
  RunIif(state, ExprType::kInt64, kIntA, kIntB, false);
}
BENCHMARK(BM_ExprIifInt);

void BM_ExprIifIntNullable(benchmark::State& state) {
  RunIif(state, ExprType::kInt64, kIntA, kIntB, true);
}
BENCHMARK(BM_ExprIifIntNullable);

void BM_ExprIifString(benchmark::State& state) {
  RunIif(state, ExprType::kString, kStringA, kStringB, false);
}
BENCHMARK(BM_ExprIifString);

void BM_ExprIifStringNullable(benchmark::State& state) {
  RunIif(state, ExprType::kString, kStringA, kStringB, true);
}
BENCHMARK(BM_ExprIifStringNullable);

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
