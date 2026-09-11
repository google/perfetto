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

#include "src/trace_processor/core/exec/tree_number_nodes.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <variant>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/exec/dataframe_scan.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// The shape of stack_profile_callsite: the id is the row, the parent is a
// sparse-null reference to an earlier row.
inline constexpr auto kIdColumn = dataframe::CreateTypedDataframeSpec(
    {"id", "parent_id"},
    dataframe::CreateTypedColumnSpec(Id{},
                                     NonNull{},
                                     IdSorted{},
                                     NoDuplicates{}),
    dataframe::CreateTypedColumnSpec(Uint32{},
                                     SparseNullWithPopcountAlways{},
                                     Unsorted{},
                                     HasDuplicates{}));

// The same tree, but with ids spread over three times their range, as they
// would be after filtering a larger table.
inline constexpr auto kScatteredIds = dataframe::CreateTypedDataframeSpec(
    {"id", "parent_id"},
    dataframe::CreateTypedColumnSpec(Uint32{},
                                     NonNull{},
                                     Unsorted{},
                                     NoDuplicates{}),
    dataframe::CreateTypedColumnSpec(Uint32{},
                                     SparseNullWithPopcountAlways{},
                                     Unsorted{},
                                     HasDuplicates{}));

std::optional<uint32_t> ParentOf(uint32_t row) {
  if (row == 0) {
    return std::nullopt;
  }
  return (row - 1) / 2;
}

void RunTreeNumberNodes(benchmark::State& state,
                        const dataframe::Dataframe& df) {
  DataframeScan scan(df, {0, 1});
  std::unique_ptr<OperatorState> scan_state = scan.MakeState();
  TreeNumberNodes op(0, 1);
  std::unique_ptr<OperatorState> op_state = op.MakeState();
  RowBatch in;
  RowBatch out;
  for (auto _ : state) {
    scan.Rewind(*scan_state);
    op.Rewind(*op_state);
    while (scan.GetData(in, *scan_state)) {
      OpResult result = op.Execute(in, out, *op_state);
      if (result == OpResult::kError) {
        state.SkipWithError(op.status(*op_state).c_message());
        return;
      }
      benchmark::DoNotOptimize(out.column(3).data());
    }
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          df.row_count());
}

dataframe::Dataframe BuildIdColumn(uint32_t rows, StringPool* pool) {
  dataframe::Dataframe df =
      dataframe::Dataframe::CreateFromTypedSpec(kIdColumn, pool);
  for (uint32_t row = 0; row < rows; ++row) {
    df.InsertUnchecked(kIdColumn, std::monostate{}, ParentOf(row));
  }
  df.Finalize();
  return df;
}

// The cost of the scan alone, to compare the operator against.
void BM_TreeNumberNodesScanOnly(benchmark::State& state) {
  StringPool pool;
  dataframe::Dataframe df =
      BuildIdColumn(static_cast<uint32_t>(state.range(0)), &pool);
  DataframeScan scan(df, {0, 1});
  std::unique_ptr<OperatorState> scan_state = scan.MakeState();
  RowBatch in;
  for (auto _ : state) {
    scan.Rewind(*scan_state);
    while (scan.GetData(in, *scan_state)) {
      benchmark::DoNotOptimize(in.column(1).data());
    }
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          df.row_count());
}
BENCHMARK(BM_TreeNumberNodesScanOnly)->Arg(10000)->Arg(100000)->Arg(1000000);

void BM_TreeNumberNodesIdColumn(benchmark::State& state) {
  StringPool pool;
  dataframe::Dataframe df =
      BuildIdColumn(static_cast<uint32_t>(state.range(0)), &pool);
  RunTreeNumberNodes(state, df);
}
BENCHMARK(BM_TreeNumberNodesIdColumn)->Arg(10000)->Arg(100000)->Arg(1000000);

void BM_TreeNumberNodesScatteredIds(benchmark::State& state) {
  StringPool pool;
  dataframe::Dataframe df =
      dataframe::Dataframe::CreateFromTypedSpec(kScatteredIds, &pool);
  const auto rows = static_cast<uint32_t>(state.range(0));
  for (uint32_t row = 0; row < rows; ++row) {
    std::optional<uint32_t> parent = ParentOf(row);
    if (parent) {
      *parent *= 3;
    }
    df.InsertUnchecked(kScatteredIds, row * 3, parent);
  }
  df.Finalize();
  RunTreeNumberNodes(state, df);
}
BENCHMARK(BM_TreeNumberNodesScatteredIds)
    ->Arg(10000)
    ->Arg(100000)
    ->Arg(1000000);

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
