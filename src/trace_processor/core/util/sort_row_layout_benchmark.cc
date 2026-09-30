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
#include <cstring>
#include <numeric>
#include <vector>

#include "perfetto/base/endian.h"
#include "src/trace_processor/core/util/ops.h"
#include "src/trace_processor/core/util/span.h"

// ORDER BY one nullable Int64 column, as a dataframe lays it out: a presence
// byte then the value, big-endian with its sign flipped. Over a million rows
// in no order, in order, in reverse (ORDER BY ... DESC of a column stored
// ascending) and in 16 ascending runs (tables of sorted rows appended), and
// over 100 rows, as when a small table is sorted over and over. Also ORDER BY
// two such columns, together differing in more bytes than one integer holds.

namespace perfetto::trace_processor::core {
namespace {

constexpr uint32_t kColumnBytes = 9;

enum class Order { kShuffled, kSorted, kReversed, kRuns };

// Lays out `value` as a present nullable Int64 key column at `to`.
void WriteKey(uint64_t value, uint8_t* to) {
  uint64_t bits = base::HostToBE64(value ^ (uint64_t{1} << 63));
  to[0] = 0xFF;
  memcpy(to + 1, &bits, sizeof(bits));
}

void Run(benchmark::State& state,
         uint32_t rows,
         Order order,
         uint32_t columns = 1) {
  const uint32_t stride = kColumnBytes * columns;
  std::vector<uint8_t> keys(size_t{rows} * stride);
  for (uint32_t i = 0; i < rows; ++i) {
    uint64_t value = 0;
    switch (order) {
      case Order::kShuffled:
        value = uint64_t{i} * 0x9E3779B97F4A7C15ull >> 20;
        break;
      case Order::kSorted:
        value = i;
        break;
      case Order::kReversed:
        value = rows - i;
        break;
      case Order::kRuns:
        value = (uint64_t{i % (rows / 16)} * 16) + (i / (rows / 16));
        break;
    }
    uint8_t* row = &keys[size_t{i} * stride];
    if (columns == 1) {
      WriteKey(value, row);
    } else {
      // A 32-bit key first, so the two differ in 10 bytes.
      WriteKey(uint64_t{i} * 2654435761u % (uint64_t{1} << 32), row);
      WriteKey(value, row + kColumnBytes);
    }
  }
  std::vector<uint32_t> indices(rows);
  for (auto _ : state) {
    std::iota(indices.begin(), indices.end(), 0u);
    Span<uint32_t> span(indices.data(), indices.data() + rows);
    ops::SortRowLayout(
        Span<const uint8_t>(keys.data(), keys.data() + keys.size()), stride,
        &span);
    benchmark::DoNotOptimize(indices.data());
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * rows);
}

void BM_SortRowLayout(benchmark::State& state) {
  Run(state, 1u << 20, Order::kShuffled);
}
BENCHMARK(BM_SortRowLayout);

void BM_SortRowLayoutSorted(benchmark::State& state) {
  Run(state, 1u << 20, Order::kSorted);
}
BENCHMARK(BM_SortRowLayoutSorted);

void BM_SortRowLayoutReversed(benchmark::State& state) {
  Run(state, 1u << 20, Order::kReversed);
}
BENCHMARK(BM_SortRowLayoutReversed);

void BM_SortRowLayoutRuns(benchmark::State& state) {
  Run(state, 1u << 20, Order::kRuns);
}
BENCHMARK(BM_SortRowLayoutRuns);

void BM_SortRowLayoutTwoColumns(benchmark::State& state) {
  Run(state, 1u << 20, Order::kShuffled, 2);
}
BENCHMARK(BM_SortRowLayoutTwoColumns);

void BM_SortRowLayoutSmall(benchmark::State& state) {
  Run(state, 100, Order::kShuffled);
}
BENCHMARK(BM_SortRowLayoutSmall);

}  // namespace
}  // namespace perfetto::trace_processor::core
