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
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/sort_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"

namespace perfetto::trace_processor::core::dataframe {
namespace {

template <typename Nullability>
constexpr auto SelectRowsSpec() {
  return CreateTypedDataframeSpec(
      {"a", "b", "c", "d"},
      CreateTypedColumnSpec(Int64{}, Nullability{}, Unsorted{}),
      CreateTypedColumnSpec(Int64{}, Nullability{}, Unsorted{}),
      CreateTypedColumnSpec(Int64{}, Nullability{}, Unsorted{}),
      CreateTypedColumnSpec(Int64{}, Nullability{}, Unsorted{}));
}

template <typename Nullability>
Dataframe MakeDataframe(StringPool* pool, uint32_t rows) {
  static constexpr auto kSpec = SelectRowsSpec<Nullability>();
  Dataframe dataframe = Dataframe::CreateFromTypedSpec(kSpec, pool);
  for (uint32_t row = 0; row < rows; ++row) {
    if constexpr (std::is_same_v<Nullability, NonNull>) {
      int64_t value = static_cast<int64_t>(row);
      dataframe.InsertUnchecked(kSpec, value, value, value, value);
    } else {
      std::optional<int64_t> value =
          row % 4 ? std::make_optional<int64_t>(row) : std::nullopt;
      dataframe.InsertUnchecked(kSpec, value, value, value, value);
    }
  }
  dataframe.Finalize();
  return dataframe;
}

template <typename Nullability>
void BM_DataframeSelectRows(benchmark::State& state) {
  const uint32_t rows = static_cast<uint32_t>(state.range(0));
  std::vector<uint32_t> selected_rows;
  selected_rows.reserve(rows / 2);
  for (uint32_t row = 0; row < rows; row += 2) {
    selected_rows.push_back(row);
  }

  StringPool pool;
  for (auto _ : state) {
    state.PauseTiming();
    Dataframe dataframe = MakeDataframe<Nullability>(&pool, rows);
    state.ResumeTiming();
    Dataframe result = std::move(dataframe).SelectRows(
        selected_rows.data(), static_cast<uint32_t>(selected_rows.size()));
    benchmark::DoNotOptimize(result);
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * rows * 4);
}

#define PERFETTO_DATAFRAME_SELECT_ROWS_BENCHMARK(nullability, rows, iters) \
  BENCHMARK_TEMPLATE(BM_DataframeSelectRows, nullability)                  \
      ->Arg(rows)                                                          \
      ->Iterations(iters)
#define PERFETTO_DATAFRAME_SELECT_ROWS_BENCHMARKS(nullability)      \
  PERFETTO_DATAFRAME_SELECT_ROWS_BENCHMARK(nullability, 16, 2000);  \
  PERFETTO_DATAFRAME_SELECT_ROWS_BENCHMARK(nullability, 256, 1000); \
  PERFETTO_DATAFRAME_SELECT_ROWS_BENCHMARK(nullability, 4096, 100); \
  PERFETTO_DATAFRAME_SELECT_ROWS_BENCHMARK(nullability, 100000, 5)
PERFETTO_DATAFRAME_SELECT_ROWS_BENCHMARKS(NonNull);
PERFETTO_DATAFRAME_SELECT_ROWS_BENCHMARKS(DenseNull);
PERFETTO_DATAFRAME_SELECT_ROWS_BENCHMARKS(SparseNull);
#undef PERFETTO_DATAFRAME_SELECT_ROWS_BENCHMARKS
#undef PERFETTO_DATAFRAME_SELECT_ROWS_BENCHMARK

}  // namespace
}  // namespace perfetto::trace_processor::core::dataframe
