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

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/exec/assert_type.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/dataframe_scan.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/tree_accumulate.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"

// What each step costs run through a pipeline, as a plan runs it: over many
// rows, where the step's own work dominates, and over one row, where what the
// pipeline does around the step does.

namespace perfetto::trace_processor::core::exec {
namespace {

inline constexpr auto kTree = dataframe::CreateTypedDataframeSpec(
    {"id", "parent_id", "value", "small"},
    dataframe::CreateTypedColumnSpec(Uint32{},
                                     NonNull{},
                                     Unsorted{},
                                     NoDuplicates{}),
    dataframe::CreateTypedColumnSpec(Uint32{},
                                     SparseNullWithPopcountAlways{},
                                     Unsorted{},
                                     HasDuplicates{}),
    dataframe::CreateTypedColumnSpec(Int64{},
                                     NonNull{},
                                     Unsorted{},
                                     HasDuplicates{}),
    dataframe::CreateTypedColumnSpec(Int32{},
                                     NonNull{},
                                     Unsorted{},
                                     HasDuplicates{}));
enum : uint32_t { kId, kParent, kValue, kSmall, kNode, kParentNode };

// A binary tree of `rows` nodes, parents first or, with `child_first`,
// children first.
dataframe::Dataframe BuildTree(uint32_t rows,
                               bool child_first,
                               StringPool* pool) {
  dataframe::Dataframe df =
      dataframe::Dataframe::CreateFromTypedSpec(kTree, pool);
  for (uint32_t row = 0; row < rows; ++row) {
    uint32_t id = child_first ? rows - 1 - row : row;
    std::optional<uint32_t> parent;
    if (id != 0) {
      parent = (id - 1) / 2;
    }
    df.InsertUnchecked(kTree, id, parent, int64_t{id % 100},
                       static_cast<int32_t>(id % 1000));
  }
  df.Finalize();
  return df;
}

// Runs the steps `make` builds over a scan of the tree, every run.
template <typename MakeSteps>
void Run(benchmark::State& state, bool child_first, MakeSteps make) {
  // Before every batch and state, which hold its buffers.
  Context context;
  auto rows = static_cast<uint32_t>(state.range(0));
  StringPool pool;
  dataframe::Dataframe df = BuildTree(rows, child_first, &pool);
  DataframeScan scan({df.shared_column(kId), df.shared_column(kParent),
                      df.shared_column(kValue), df.shared_column(kSmall)},
                     df.row_count());
  Pipeline pipeline(scan, make(), {});
  std::unique_ptr<OperatorState> run = pipeline.MakeState(context);
  RowBatch scratch;
  for (auto _ : state) {
    pipeline.Rewind(*run);
    while (RowBatch* batch = pipeline.Next(scratch, *run)) {
      benchmark::DoNotOptimize(batch->size());
    }
  }
  if (!pipeline.status(*run).ok()) {
    state.SkipWithError(pipeline.status(*run).c_message());
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * rows);
}

void Sizes(benchmark::internal::Benchmark* b) {
  b->Arg(1)->Arg(1 << 20);
}

void BM_StepsScanOnly(benchmark::State& state) {
  Run(state, false, [] { return std::vector<Pipeline::Step>(); });
}
BENCHMARK(BM_StepsScanOnly)->Apply(Sizes);

void BM_StepsTreeNumberNodes(benchmark::State& state) {
  Run(state, false, [] {
    std::vector<Pipeline::Step> steps;
    steps.push_back(std::make_unique<TreeNumberNodes>(kId, kParent));
    return steps;
  });
}
BENCHMARK(BM_StepsTreeNumberNodes)->Apply(Sizes);

void BM_StepsTreeAccumulateDown(benchmark::State& state) {
  Run(state, false, [] {
    std::vector<Pipeline::Step> steps;
    steps.push_back(std::make_unique<TreeNumberNodes>(kId, kParent));
    steps.push_back(std::make_unique<TreeAccumulateDown>(
        TreeAccumulateSpec{kNode, kParentNode, kValue}));
    return steps;
  });
}
BENCHMARK(BM_StepsTreeAccumulateDown)->Apply(Sizes);

void BM_StepsTreeAccumulateUp(benchmark::State& state) {
  Run(state, true, [] {
    std::vector<Pipeline::Step> steps;
    steps.push_back(std::make_unique<TreeNumberNodes>(kId, kParent));
    steps.push_back(std::make_unique<TreeAccumulateUp>(
        TreeAccumulateSpec{kNode, kParentNode, kValue}));
    return steps;
  });
}
BENCHMARK(BM_StepsTreeAccumulateUp)->Apply(Sizes);

// A column already of the type asserted passes through unchanged.
void BM_StepsAssertTypeAsIs(benchmark::State& state) {
  Run(state, false, [] {
    std::vector<Pipeline::Step> steps;
    steps.push_back(std::make_unique<AssertType>(
        kValue, AssertTypeTarget(Int64{}), "value"));
    return steps;
  });
}
BENCHMARK(BM_StepsAssertTypeAsIs)->Apply(Sizes);

// A narrower integer column is widened into one of the type asserted.
void BM_StepsAssertTypeWiden(benchmark::State& state) {
  Run(state, false, [] {
    std::vector<Pipeline::Step> steps;
    steps.push_back(std::make_unique<AssertType>(
        kSmall, AssertTypeTarget(Int64{}), "small"));
    return steps;
  });
}
BENCHMARK(BM_StepsAssertTypeWiden)->Apply(Sizes);

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
