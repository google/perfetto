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

#include "src/trace_processor/core/util/ops.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::ops {
namespace {

enum class GatherPattern {
  kCompact,
  kRandom,
};

template <typename T, GatherPattern Pattern, bool InPlace>
void BM_GatherRows(benchmark::State& state) {
  const uint32_t output_rows = static_cast<uint32_t>(state.range(0));
  const uint32_t source_size =
      Pattern == GatherPattern::kCompact ? output_rows * 2 : output_rows;
  std::vector<T> source(source_size);
  std::vector<T> output(InPlace ? 0 : output_rows);
  std::vector<uint32_t> source_rows(output_rows);

  std::minstd_rand0 random(42);
  for (uint32_t row = 0; row < source_size; ++row) {
    source[row] = static_cast<T>(row);
  }
  for (uint32_t row = 0; row < output_rows; ++row) {
    if constexpr (Pattern == GatherPattern::kCompact) {
      source_rows[row] = row * 2;
    } else {
      static_assert(!InPlace, "random mappings cannot gather in place");
      source_rows[row] = random() % source_size;
    }
  }

  for (auto _ : state) {
    Span<T> destination =
        InPlace ? MakeMutableSpan(source) : MakeMutableSpan(output);
    GatherRows(MakeSpan(source), destination, MakeSpan(source_rows));
    benchmark::DoNotOptimize(destination);
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          output_rows);
}

#define PERFETTO_GATHER_ROWS_BENCHMARK(type, pattern, in_place) \
  BENCHMARK_TEMPLATE(BM_GatherRows, type, pattern, in_place)->Arg(100000)
PERFETTO_GATHER_ROWS_BENCHMARK(uint32_t, GatherPattern::kCompact, false);
PERFETTO_GATHER_ROWS_BENCHMARK(uint32_t, GatherPattern::kCompact, true);
PERFETTO_GATHER_ROWS_BENCHMARK(uint32_t, GatherPattern::kRandom, false);
PERFETTO_GATHER_ROWS_BENCHMARK(int64_t, GatherPattern::kCompact, false);
PERFETTO_GATHER_ROWS_BENCHMARK(int64_t, GatherPattern::kCompact, true);
PERFETTO_GATHER_ROWS_BENCHMARK(int64_t, GatherPattern::kRandom, false);
#undef PERFETTO_GATHER_ROWS_BENCHMARK

}  // namespace
}  // namespace perfetto::trace_processor::core::ops
