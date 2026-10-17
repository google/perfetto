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
#include <random>
#include <vector>

#include "src/trace_processor/util/sort.h"

// Sorts by key, as most of trace processor's sorts are, across the sizes its
// callers see: a few elements in query planning, thousands in importers and
// millions of log lines or events.

namespace perfetto::trace_processor {
namespace {

enum Keys : int64_t {
  // Any 64-bit value.
  kRandom = 0,
  // Timestamps in one trace: only the low bits differ.
  kTimestamps = 1,
  // Ascending, with one in 64 elements out of place, as log lines are.
  kNearlySorted = 2,
};

template <size_t kBytes>
struct Element {
  int64_t key;
  char payload[kBytes - sizeof(int64_t)];
};

template <size_t kBytes>
void BM_SortByKey(benchmark::State& state) {
  auto keys = static_cast<Keys>(state.range(0));
  auto size = static_cast<uint32_t>(state.range(1));
  std::mt19937_64 rnd(1);
  std::vector<Element<kBytes>> input(size);
  for (uint32_t i = 0; i < size; ++i) {
    switch (keys) {
      case kRandom:
        input[i].key = static_cast<int64_t>(rnd());
        break;
      case kTimestamps:
        input[i].key =
            350000000000000 + static_cast<int64_t>(rnd() % 30000000000);
        break;
      case kNearlySorted:
        input[i].key = int64_t{i} * 1000;
        break;
    }
  }
  if (keys == kNearlySorted) {
    for (uint32_t i = 0; i + 1 < size; i += 64) {
      std::swap(input[i].key,
                input[i + 1 + rnd() % std::min(63u, size - i - 1)].key);
    }
  }
  std::vector<Element<kBytes>> data;
  for (auto _ : state) {
    data = input;
    StableSortByKey(data.begin(), data.end(),
                    [](const Element<kBytes>& e) { return e.key; });
    benchmark::DoNotOptimize(data.data());
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * size);
}

void Args(benchmark::internal::Benchmark* b) {
  b->ArgsProduct(
      {{kRandom, kTimestamps, kNearlySorted}, {16, 64, 1000, 100000, 1000000}});
}
BENCHMARK_TEMPLATE(BM_SortByKey, 16)->Apply(Args);
BENCHMARK_TEMPLATE(BM_SortByKey, 64)->Apply(Args);

}  // namespace
}  // namespace perfetto::trace_processor
