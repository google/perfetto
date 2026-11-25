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
#include "src/trace_processor/core/exec/tree_order.h"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <utility>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/exec/dataframe_scan.h"
#include "src/trace_processor/core/exec/expand.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"
#include "src/trace_processor/core/util/arena.h"

namespace perfetto::trace_processor::core::exec {
namespace {

inline constexpr auto kTree = dataframe::CreateTypedDataframeSpec(
    {"id", "parent_id"},
    dataframe::CreateTypedColumnSpec(Uint32{},
                                     NonNull{},
                                     Unsorted{},
                                     NoDuplicates{}),
    dataframe::CreateTypedColumnSpec(Uint32{},
                                     SparseNullWithPopcountAlways{},
                                     Unsorted{},
                                     HasDuplicates{}));

enum class Arrival : int64_t { kParentFirst, kChildFirst, kShuffled };

std::optional<uint32_t> ParentOf(uint32_t id) {
  if (id == 0) {
    return std::nullopt;
  }
  return (id - 1) / 2;
}

// A binary tree of `rows` nodes whose rows arrive in the given order.
dataframe::Dataframe BuildTree(uint32_t rows,
                               Arrival arrival,
                               StringPool* pool) {
  std::vector<uint32_t> ids(rows);
  for (uint32_t i = 0; i < rows; ++i) {
    ids[i] = i;
  }
  if (arrival == Arrival::kChildFirst) {
    std::reverse(ids.begin(), ids.end());
  } else if (arrival == Arrival::kShuffled) {
    std::mt19937 rng(7);
    std::shuffle(ids.begin(), ids.end(), rng);
  }
  dataframe::Dataframe df =
      dataframe::Dataframe::CreateFromTypedSpec(kTree, pool);
  for (uint32_t id : ids) {
    df.InsertUnchecked(kTree, id, ParentOf(id));
  }
  df.Finalize();
  return df;
}

void Run(benchmark::State& state,
         const Source& source,
         uint32_t rows,
         const Pipelines* pipelines) {
  // Before every batch and state, which hold its buffers.
  Context context;
  context.SetPipelines(pipelines);
  std::unique_ptr<OperatorState> run = source.MakeState(context);
  RowBatch out;
  for (auto _ : state) {
    Restart(source, *run);
    while (source.GetData(out, *run)) {
      benchmark::DoNotOptimize(out.column(2).data());
    }
    if (!source.status(*run).ok()) {
      state.SkipWithError(source.status(*run).c_message());
      return;
    }
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * rows);
}

// Numbering alone, to compare the ordering against.
void BM_TreeOrderNumberOnly(benchmark::State& state) {
  core::Arena arena;
  StringPool pool;
  auto rows = static_cast<uint32_t>(state.range(0));
  dataframe::Dataframe df =
      BuildTree(rows, static_cast<Arrival>(state.range(1)), &pool);
  DataframeScan scan(
      arena.CopySpan<const dataframe::Column*>({&df.column(0), &df.column(1)}),
      df.row_count(), ReadDirection::kForwards);
  std::vector<Pipeline::Step> ops;
  ops.push_back(arena.New<Expand>(arena.CopySpan<uint32_t>({1})));
  ops.push_back(arena.New<TreeNumberNodes>(0u, 1u));
  Pipeline pipeline(arena, scan, ops);
  Run(state, pipeline, rows, nullptr);
}

void BM_TreeParentFirst(benchmark::State& state) {
  core::Arena arena;
  StringPool pool;
  auto rows = static_cast<uint32_t>(state.range(0));
  dataframe::Dataframe df =
      BuildTree(rows, static_cast<Arrival>(state.range(1)), &pool);
  DataframeScan scan(
      arena.CopySpan<const dataframe::Column*>({&df.column(0), &df.column(1)}),
      df.row_count(), ReadDirection::kForwards);
  std::vector<Pipeline::Step> ops;
  ops.push_back(arena.New<Expand>(arena.CopySpan<uint32_t>({1})));
  ops.push_back(arena.New<TreeNumberNodes>(0u, 1u));
  ops.push_back(arena.New<TreeParentFirst>(2u, 3u));
  Pipeline pipeline(arena, scan, ops);
  Run(state, pipeline, rows, nullptr);
}

void BM_TreeChildFirst(benchmark::State& state) {
  core::Arena arena;
  StringPool pool;
  auto rows = static_cast<uint32_t>(state.range(0));
  dataframe::Dataframe df =
      BuildTree(rows, static_cast<Arrival>(state.range(1)), &pool);
  DataframeScan scan(
      arena.CopySpan<const dataframe::Column*>({&df.column(0), &df.column(1)}),
      df.row_count(), ReadDirection::kForwards);
  std::vector<Pipeline::Step> ops;
  ops.push_back(arena.New<Expand>(arena.CopySpan<uint32_t>({1})));
  ops.push_back(arena.New<TreeNumberNodes>(0u, 1u));
  // A plan ends the pipeline in the breaker, and serves it to the next.
  Pipelines pipelines;
  const TreeChildFirst* breaker = arena.New<TreeChildFirst>(2u, 3u);
  uint32_t id = pipelines.Add(arena.New<Pipeline>(arena, scan, ops), breaker);
  Served order(id, *breaker);
  Run(state, order, rows, &pipelines);
}

void Arrivals(benchmark::internal::Benchmark* b) {
  for (int64_t rows : {10000, 1000000}) {
    for (Arrival arrival :
         {Arrival::kParentFirst, Arrival::kChildFirst, Arrival::kShuffled}) {
      b->Args({rows, static_cast<int64_t>(arrival)});
    }
  }
}

BENCHMARK(BM_TreeOrderNumberOnly)->Apply(Arrivals);
BENCHMARK(BM_TreeParentFirst)->Apply(Arrivals);
BENCHMARK(BM_TreeChildFirst)->Apply(Arrivals);

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
