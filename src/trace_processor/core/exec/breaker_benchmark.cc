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
#include <string>
#include <utility>
#include <vector>

#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/containers/string_pool.h"

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/grouped_sort.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/sort.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/exec/top_k.h"
#include "src/trace_processor/core/util/arena.h"

// Runs Sort and GroupedSort on 16 full batches and on a single row. These
// stand for a large input and for a small input that is run many times. Runs
// TopK, against Sort, on 512 batches.

namespace perfetto::trace_processor::core::exec {
namespace {

constexpr uint32_t kRows = 16 * kMaxBatchRows;

struct Column {
  StorageType type;
  const void* data;
};

// Runs `breaker` over `rows` rows of `columns`.
void RunColumns(benchmark::State& state,
                core::Arena& arena,
                const Breaker* breaker,
                uint32_t rows,
                const std::vector<Column>& columns) {
  std::vector<RowBatch> batches;
  for (uint32_t at = 0; at < rows; at += kMaxBatchRows) {
    uint32_t count = std::min(kMaxBatchRows, rows - at);
    RowBatch& batch = batches.emplace_back();
    for (const Column& column : columns) {
      batch.AddBorrowedColumn(
          ColumnView::Reference(column.type, column.data, nullptr, at));
    }
    batch.SetRowCount(count);
  }
  test::BreakerRun run(arena, batches, breaker);
  for (auto _ : state) {
    benchmark::DoNotOptimize(run.Run());
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * rows);
}

// Runs `breaker` over `rows` rows of `key_columns` Int64 keys with `distinct`
// values spread through them.
void Run(benchmark::State& state,
         core::Arena& arena,
         const Breaker* breaker,
         uint32_t rows,
         uint32_t distinct,
         uint32_t key_columns = 1) {
  std::vector<int64_t> keys;
  for (uint32_t i = 0; i < rows; ++i) {
    keys.push_back(int64_t{(i * 2654435761u) % distinct});
  }
  RunColumns(
      state, arena, breaker, rows,
      std::vector<Column>(key_columns, {StorageType{Int64{}}, keys.data()}));
}

constexpr uint32_t kKeepRows = 512 * kMaxBatchRows;

enum KeepInput : int64_t { kRandom, kAscending, kDescending, kFewDistinct };

std::vector<int64_t> KeepKeys(int64_t input) {
  std::vector<int64_t> keys(kKeepRows);
  for (uint32_t i = 0; i < kKeepRows; ++i) {
    uint32_t spread = i * 2654435761u;
    switch (input) {
      case kRandom:
        keys[i] = spread % (1u << 30);
        break;
      case kAscending:
        keys[i] = i;
        break;
      case kDescending:
        keys[i] = kKeepRows - i;
        break;
      default:
        keys[i] = spread % 16;
        break;
    }
  }
  return keys;
}

// A TopK of `count` rows, or with 0, a Sort.
const Breaker* SortFirst(core::Arena& arena,
                         int64_t count,
                         const StringPool* pool) {
  if (count == 0) {
    return arena.New<Sort>(
        SortSpec{arena.CopySpan<SortKey>({{0, false}}), pool});
  }
  return arena.New<TopK>(TopKSpec{arena.CopySpan<SortKey>({{0, false}}), pool,
                                  static_cast<uint32_t>(count)});
}

void BM_SortInt64(benchmark::State& state) {
  core::Arena arena;
  Run(state, arena,
      arena.New<Sort>(SortSpec{arena.CopySpan<SortKey>({{0, false}})}), kRows,
      1u << 30);
}
BENCHMARK(BM_SortInt64);

void BM_SortInt64OneRow(benchmark::State& state) {
  core::Arena arena;
  Run(state, arena,
      arena.New<Sort>(SortSpec{arena.CopySpan<SortKey>({{0, false}})}), 1, 1);
}
BENCHMARK(BM_SortInt64OneRow);

// Args: rows kept, with 0 for a full sort; the KeepInput; Int64 columns
// carried beside the key.
void BM_TopKInt64(benchmark::State& state) {
  core::Arena arena;
  const Breaker* op = SortFirst(arena, state.range(0), nullptr);
  std::vector<int64_t> keys = KeepKeys(state.range(1));
  std::vector<Column> columns(1 + static_cast<size_t>(state.range(2)),
                              {StorageType{Int64{}}, keys.data()});
  RunColumns(state, arena, op, kKeepRows, columns);
}
BENCHMARK(BM_TopKInt64)
    ->ArgNames({"keep", "input", "payload"})
    ->ArgsProduct({{0, 1, 100, 10000, 100000},
                   {kRandom, kAscending, kDescending, kFewDistinct},
                   {0, 4}});

// Args: rows kept, with 0 for a full sort; the KeepInput.
void BM_TopKString(benchmark::State& state) {
  StringPool pool;
  std::vector<StringPool::Id> keys;
  for (int64_t key : KeepKeys(state.range(1))) {
    keys.push_back(
        pool.InternString(base::StringView("key" + std::to_string(key))));
  }
  core::Arena arena;
  const Breaker* op = SortFirst(arena, state.range(0), &pool);
  RunColumns(state, arena, op, kKeepRows,
             {{StorageType{String{}}, keys.data()}});
}
BENCHMARK(BM_TopKString)
    ->ArgNames({"keep", "input"})
    ->ArgsProduct({{0, 1, 100, 10000}, {kRandom, kDescending, kFewDistinct}});

void BM_GroupByInt64(benchmark::State& state) {
  core::Arena arena;
  Run(state, arena,
      arena.New<GroupedSort>(
          GroupedSortSpec{arena.CopySpan<uint32_t>({0}), {}}),
      kRows, static_cast<uint32_t>(state.range(0)));
}
BENCHMARK(BM_GroupByInt64)->Arg(16)->Arg(kRows);

// Keys too long for std::string to hold inline.
void BM_GroupByThreeKeys(benchmark::State& state) {
  core::Arena arena;
  Run(state, arena,
      arena.New<GroupedSort>(
          GroupedSortSpec{arena.CopySpan<uint32_t>({0, 1, 2}), {}}),
      kRows, kRows, 3);
}
BENCHMARK(BM_GroupByThreeKeys);

void BM_GroupByInt64OneRow(benchmark::State& state) {
  core::Arena arena;
  Run(state, arena,
      arena.New<GroupedSort>(
          GroupedSortSpec{arena.CopySpan<uint32_t>({0}), {}}),
      1, 1);
}
BENCHMARK(BM_GroupByInt64OneRow);

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
