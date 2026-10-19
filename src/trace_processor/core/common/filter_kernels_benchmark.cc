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
#include <random>
#include <vector>

#include "src/trace_processor/core/common/filter_kernels.h"
#include "src/trace_processor/core/util/range.h"

namespace perfetto::trace_processor::core {
namespace {

constexpr uint32_t kRows = 1 << 20;
constexpr uint32_t kLookups = 1024;

// Each value repeats `run` times, so equal ranges have that length.
std::vector<uint32_t> SortedRuns(uint32_t run) {
  std::vector<uint32_t> data(kRows);
  for (uint32_t i = 0; i < kRows; ++i) {
    data[i] = i / run;
  }
  return data;
}

std::vector<uint32_t> RandomValues(uint32_t max) {
  std::mt19937 rng(1);
  std::uniform_int_distribution<uint32_t> dist(0, max);
  std::vector<uint32_t> values(kLookups);
  for (uint32_t& v : values) {
    v = dist(rng);
  }
  return values;
}

// Argument: the length of each equal range.
void BM_SortedEqualRange(benchmark::State& state) {
  auto run = static_cast<uint32_t>(state.range(0));
  std::vector<uint32_t> data = SortedRuns(run);
  std::vector<uint32_t> values = RandomValues(kRows / run - 1);
  for (auto _ : state) {
    for (uint32_t v : values) {
      Range range{0, kRows};
      filter::SortedEqualRange(data.data(), v, nullptr, range);
      benchmark::DoNotOptimize(range);
    }
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kLookups);
}
BENCHMARK(BM_SortedEqualRange)->RangeMultiplier(4)->Range(1, 1 << 16);

// Argument: the length of each set. A SetId column holds, at each row, the
// row its set starts at.
void BM_SetIdSortedEqualRange(benchmark::State& state) {
  auto run = static_cast<uint32_t>(state.range(0));
  std::vector<uint32_t> data(kRows);
  for (uint32_t i = 0; i < kRows; ++i) {
    data[i] = i / run * run;
  }
  std::vector<uint32_t> values = RandomValues(kRows / run - 1);
  for (uint32_t& v : values) {
    v *= run;
  }
  for (auto _ : state) {
    for (uint32_t v : values) {
      Range range{0, kRows};
      filter::SetIdSortedEqualRange(data.data(), v, range);
      benchmark::DoNotOptimize(range);
    }
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kLookups);
}
BENCHMARK(BM_SetIdSortedEqualRange)->RangeMultiplier(4)->Range(1, 1 << 16);

}  // namespace
}  // namespace perfetto::trace_processor::core
