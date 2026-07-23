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
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/plugins/flamegraph/flamegraph.h"

namespace perfetto::trace_processor::flamegraph {
namespace {

// Deterministic LCG so runs are comparable.
struct Rng {
  uint64_t state = 42;
  uint32_t operator()() {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<uint32_t>(state >> 33);
  }
};

// A forest plus the per-frame property columns the collection step would
// intern alongside it: |mapping| groups (and is folded into the merge
// key), |source_file| aggregates.
struct BenchInput {
  Forest forest;
  std::vector<StringPool::Id> mapping;
  std::vector<StringPool::Id> source_file;
};

BenchInput GenForest(StringPool* pool,
                     uint32_t n,
                     uint32_t metric_count,
                     bool with_properties) {
  constexpr uint32_t kNameCardinality = 10000;
  constexpr uint32_t kMappingCardinality = 40;
  constexpr uint32_t kSourceFileCardinality = 2000;

  Rng rng;
  BenchInput in;
  Forest& forest = in.forest;
  forest.metric_count = metric_count;
  for (uint32_t t = 0; t < kNameCardinality; ++t) {
    forest.name_table.push_back(pool->InternString(
        base::StringView("frame_" + std::to_string(t))));
  }
  std::vector<StringPool::Id> mappings;
  for (uint32_t t = 0; t < kMappingCardinality && with_properties; ++t) {
    mappings.push_back(pool->InternString(
        base::StringView("mapping_" + std::to_string(t))));
  }
  std::vector<StringPool::Id> files;
  for (uint32_t t = 0; t < kSourceFileCardinality && with_properties; ++t) {
    files.push_back(pool->InternString(
        base::StringView("file_" + std::to_string(t))));
  }
  std::vector<uint32_t> stack;
  for (uint32_t i = 0; i < n; ++i) {
    if (!stack.empty() && rng() % 100 < 40) {
      stack.resize(stack.size() - (rng() % stack.size()));
    }
    forest.parent.push_back(stack.empty() ? kNoParent : stack.back());
    uint32_t name = rng() % kNameCardinality;
    forest.name.push_back(name);
    // The merge key folds the grouping properties in; the generated
    // mapping is a function of the name, so the name index is exactly the
    // distinct (name, mapping) id.
    forest.key.push_back(name);
    for (uint32_t m = 0; m < metric_count; ++m) {
      forest.metrics.push_back(rng() % 3 == 0 ? rng() % 100 : 0);
    }
    if (with_properties) {
      in.mapping.push_back(mappings[name % kMappingCardinality]);
      in.source_file.push_back(files[rng() % kSourceFileCardinality]);
    }
    stack.push_back(i);
  }
  return in;
}

// Converts the trees to a flat dataframe like the library's ToDataframe,
// additionally emitting the grouping property of the representative frame
// and the ONE_OR_SUMMARY aggregation of the constituents' values: the
// work the SQL intrinsic glue does for property columns.
constexpr auto PropsRowSpec() {
  using core::dataframe::CreateTypedColumnSpec;
  using core::dataframe::Int64;
  using core::dataframe::NonNull;
  using core::dataframe::String;
  using core::dataframe::Unsorted;
  return core::dataframe::CreateTypedDataframeSpec(
      {"id", "parentId", "depth", "name", "mapping", "sourceFile",
       "selfValue", "cumulativeValue"},
      CreateTypedColumnSpec(Int64{}, NonNull{}, Unsorted{}),
      CreateTypedColumnSpec(Int64{}, NonNull{}, Unsorted{}),
      CreateTypedColumnSpec(Int64{}, NonNull{}, Unsorted{}),
      CreateTypedColumnSpec(String{}, NonNull{}, Unsorted{}),
      CreateTypedColumnSpec(String{}, NonNull{}, Unsorted{}),
      CreateTypedColumnSpec(String{}, NonNull{}, Unsorted{}),
      CreateTypedColumnSpec(Int64{}, NonNull{}, Unsorted{}),
      CreateTypedColumnSpec(Int64{}, NonNull{}, Unsorted{}));
}

core::dataframe::Dataframe TreesToDataframeWithProps(const Flamegraph& fg,
                                                     const BenchInput& in,
                                                     StringPool* pool) {
  static constexpr auto kSpec = PropsRowSpec();
  auto df = core::dataframe::Dataframe::CreateFromTypedSpec(kSpec, pool);
  std::vector<StringPool::Id> seen;
  int64_t base = 0;
  for (const Tree* tree : {&fg.down, &fg.up}) {
    uint32_t k = tree->metric_count;
    std::vector<int64_t> depth(tree->size());
    for (uint32_t i = 0; i < tree->size(); ++i) {
      bool root = tree->parent[i] == kNoParent;
      depth[i] = root ? 1 : depth[tree->parent[i]] + 1;
      // ONE_OR_SUMMARY: the representative frame's value, with a summary
      // suffix when the merged frames had several distinct ones. The
      // double space and the count including the shown value mirror the
      // SQL surface this replaces.
      seen.clear();
      for (uint32_t c = tree->constituents_offset[i];
           c < tree->constituents_offset[i + 1]; ++c) {
        StringPool::Id file = in.source_file[tree->constituents[c]];
        if (std::find(seen.begin(), seen.end(), file) == seen.end()) {
          seen.push_back(file);
        }
      }
      StringPool::Id file = in.source_file[tree->rep_frame[i]];
      if (seen.size() > 1) {
        std::string text = pool->Get(file).ToStdString() + "  and " +
                           std::to_string(seen.size()) + " others";
        file = pool->InternString(base::StringView(text));
      }
      df.InsertUnchecked(
          kSpec, base + i, root ? -1 : base + tree->parent[i],
          tree == &fg.up ? -depth[i] : depth[i],
          in.forest.name_table[in.forest.name[tree->rep_frame[i]]],
          in.mapping[tree->rep_frame[i]], file,
          static_cast<int64_t>(tree->self[i * k]),
          static_cast<int64_t>(tree->cumulative[i * k]));
    }
    base += static_cast<int64_t>(tree->size());
  }
  df.Finalize();
  return df;
}

void Run(benchmark::State& state,
         const BenchInput& in,
         const Config& config,
         StringPool* pool) {
  for (auto _ : state) {
    auto fg = Build(in.forest, config, *pool);
    PERFETTO_CHECK(fg.ok());
    benchmark::DoNotOptimize(fg);
  }
  state.counters["frames/s"] =
      benchmark::Counter(static_cast<double>(in.forest.size()),
                         benchmark::Counter::kIsIterationInvariantRate);
}

// Build plus the dataframe conversion (and property aggregation when the
// input has properties).
void RunWithRows(benchmark::State& state,
                 const BenchInput& in,
                 const Config& config,
                 StringPool* pool) {
  for (auto _ : state) {
    auto fg = Build(in.forest, config, *pool);
    PERFETTO_CHECK(fg.ok());
    if (in.mapping.empty()) {
      auto df = ToDataframe(*fg, in.forest, pool);
      PERFETTO_CHECK(df.ok());
      benchmark::DoNotOptimize(df);
    } else {
      auto df = TreesToDataframeWithProps(*fg, in, pool);
      benchmark::DoNotOptimize(df);
    }
  }
  state.counters["frames/s"] =
      benchmark::Counter(static_cast<double>(in.forest.size()),
                         benchmark::Counter::kIsIterationInvariantRate);
}

void BM_FlamegraphTreeTopDown(benchmark::State& state) {
  StringPool pool;
  BenchInput in =
      GenForest(&pool, static_cast<uint32_t>(state.range(0)), 1, false);
  Run(state, in, Config{}, &pool);
}
BENCHMARK(BM_FlamegraphTreeTopDown)->Arg(100000)->Arg(1000000);

void BM_FlamegraphTreeTopDown4Metrics(benchmark::State& state) {
  StringPool pool;
  BenchInput in =
      GenForest(&pool, static_cast<uint32_t>(state.range(0)), 4, false);
  Run(state, in, Config{}, &pool);
}
BENCHMARK(BM_FlamegraphTreeTopDown4Metrics)->Arg(100000)->Arg(1000000);

void BM_FlamegraphTreeTopDownFiltered(benchmark::State& state) {
  StringPool pool;
  BenchInput in =
      GenForest(&pool, static_cast<uint32_t>(state.range(0)), 1, false);
  Config config;
  config.filters.push_back({Config::FilterKind::kShowStack, "frame_1\\d\\d"});
  config.filters.push_back({Config::FilterKind::kHideFrame, "frame_2\\d\\d"});
  Run(state, in, config, &pool);
}
BENCHMARK(BM_FlamegraphTreeTopDownFiltered)->Arg(100000)->Arg(1000000);

void BM_FlamegraphTreeBottomUp(benchmark::State& state) {
  StringPool pool;
  BenchInput in =
      GenForest(&pool, static_cast<uint32_t>(state.range(0)), 1, false);
  Config config;
  config.view = Config::View::kBottomUp;
  Run(state, in, config, &pool);
}
BENCHMARK(BM_FlamegraphTreeBottomUp)->Arg(100000)->Arg(1000000);

void BM_FlamegraphTreePivot(benchmark::State& state) {
  StringPool pool;
  BenchInput in =
      GenForest(&pool, static_cast<uint32_t>(state.range(0)), 1, false);
  Config config;
  config.view = Config::View::kPivot;
  config.pivot_pattern = "frame_12";
  Run(state, in, config, &pool);
}
BENCHMARK(BM_FlamegraphTreePivot)->Arg(100000)->Arg(1000000);

void BM_FlamegraphTreeTopDownRows(benchmark::State& state) {
  StringPool pool;
  BenchInput in =
      GenForest(&pool, static_cast<uint32_t>(state.range(0)), 1, false);
  RunWithRows(state, in, Config{}, &pool);
}
BENCHMARK(BM_FlamegraphTreeTopDownRows)->Arg(100000)->Arg(1000000);

void BM_FlamegraphTreeTopDownPropertiesRows(benchmark::State& state) {
  StringPool pool;
  BenchInput in =
      GenForest(&pool, static_cast<uint32_t>(state.range(0)), 1, true);
  RunWithRows(state, in, Config{}, &pool);
}
BENCHMARK(BM_FlamegraphTreeTopDownPropertiesRows)->Arg(100000)->Arg(1000000);

void BM_FlamegraphTreeBottomUpRows(benchmark::State& state) {
  StringPool pool;
  BenchInput in =
      GenForest(&pool, static_cast<uint32_t>(state.range(0)), 1, false);
  Config config;
  config.view = Config::View::kBottomUp;
  RunWithRows(state, in, config, &pool);
}
BENCHMARK(BM_FlamegraphTreeBottomUpRows)->Arg(100000)->Arg(1000000);

void BM_FlamegraphTreePivotRows(benchmark::State& state) {
  StringPool pool;
  BenchInput in =
      GenForest(&pool, static_cast<uint32_t>(state.range(0)), 1, false);
  Config config;
  config.view = Config::View::kPivot;
  config.pivot_pattern = "frame_12";
  RunWithRows(state, in, config, &pool);
}
BENCHMARK(BM_FlamegraphTreePivotRows)->Arg(100000)->Arg(1000000);

}  // namespace
}  // namespace perfetto::trace_processor::flamegraph
