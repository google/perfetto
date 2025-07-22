/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/dataframe/impl/sort.h"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace {

// A simple POD object used for benchmarking LSD radix sort.
struct PodObject {
  uint64_t key;
  uint32_t value;
};

// A trivially copyable struct that points to string data. This is used for
// benchmarking MSD radix sort, which requires trivially copyable elements.
struct StringPtr {
  const char* data;
  size_t size;
};

// Generates a random alphanumeric string of a given length.
std::string RandomString(std::mt19937& gen, size_t len) {
  std::string str(len, 0);
  std::uniform_int_distribution<> dist(32, 126);  // printable ascii
  for (size_t i = 0; i < len; ++i) {
    str[i] = static_cast<char>(dist(gen));
  }
  return str;
}

bool IsBenchmarkFunctionalOnly() {
  return getenv("BENCHMARK_FUNCTIONAL_TEST_ONLY") != nullptr;
}

// --- Benchmarks for LSD Radix Sort ---

// Benchmarks the performance of LSD RadixSort on objects with uint64_t keys.
static void RadixSortLsdArgs(benchmark::internal::Benchmark* b) {
  if (IsBenchmarkFunctionalOnly()) {
    b->Arg(16);
  } else {
    for (int i = 16; i <= 1 << 22; i *= 64) {
      b->Arg(i);
    }
  }
}

static void BM_RadixSortLsd(benchmark::State& state) {
  const auto n = static_cast<size_t>(state.range(0));

  std::vector<PodObject> data(n);
  std::mt19937_64 engine(0);
  std::uniform_int_distribution<uint64_t> dist;
  for (size_t i = 0; i < n; ++i) {
    data[i] = {dist(engine), static_cast<uint32_t>(i)};
  }

  std::vector<PodObject> scratch(n);
  std::vector<uint32_t> counts(1 << 16);

  for (auto _ : state) {
    std::vector<PodObject> working_copy = data;
    perfetto::base::RadixSort(
        working_copy.data(), working_copy.data() + n, scratch.data(),
        counts.data(), sizeof(uint64_t), [](const PodObject& obj) {
          return reinterpret_cast<const uint8_t*>(&obj.key);
        });
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * n));
}
BENCHMARK(BM_RadixSortLsd)->Apply(RadixSortLsdArgs);

// Baseline benchmark using std::sort for comparison with LSD RadixSort.
static void BM_RadixSortLsdStd(benchmark::State& state) {
  const auto n = static_cast<size_t>(state.range(0));

  std::vector<PodObject> data(n);
  std::mt19937_64 engine(0);
  std::uniform_int_distribution<uint64_t> dist;
  for (size_t i = 0; i < n; ++i) {
    data[i] = {dist(engine), static_cast<uint32_t>(i)};
  }

  for (auto _ : state) {
    std::vector<PodObject> working_copy = data;
    std::stable_sort(
        working_copy.begin(), working_copy.end(),
        [](const PodObject& a, const PodObject& b) { return a.key < b.key; });
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * n));
}
BENCHMARK(BM_RadixSortLsdStd)->Apply(RadixSortLsdArgs);

// --- Benchmarks for MSD Radix Sort ---

// Benchmarks the performance of MSD RadixSort on string keys.
static void RadixSortMsdArgs(benchmark::internal::Benchmark* b) {
  if (IsBenchmarkFunctionalOnly()) {
    b->Args({16, 8});
  } else {
    for (int i = 16; i <= 1 << 22; i *= 64) {
      for (int j : {8, 64}) {
        b->Args({i, j});
      }
    }
  }
}

static void BM_RadixSortMsd(benchmark::State& state) {
  const auto n = static_cast<size_t>(state.range(0));
  const auto str_len = static_cast<size_t>(state.range(1));

  std::vector<std::string> string_data(n);
  std::vector<StringPtr> data(n);
  std::mt19937 engine(0);
  for (size_t i = 0; i < n; ++i) {
    string_data[i] = RandomString(engine, str_len);
    data[i] = {string_data[i].data(), string_data[i].size()};
  }

  std::vector<StringPtr> scratch(n);

  for (auto _ : state) {
    std::vector<StringPtr> working_copy = data;
    perfetto::base::MsdRadixSort(
        working_copy.data(), working_copy.data() + n, scratch.data(),
        [](const StringPtr& s) { return std::string_view(s.data, s.size); });
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * n));
}
BENCHMARK(BM_RadixSortMsd)->Apply(RadixSortMsdArgs);

// Baseline benchmark using std::sort for comparison with MSD RadixSort.
static void BM_RadixSortStdStringPtr(benchmark::State& state) {
  const auto n = static_cast<size_t>(state.range(0));
  const auto str_len = static_cast<size_t>(state.range(1));

  std::vector<std::string> string_data(n);
  std::vector<StringPtr> data(n);
  std::mt19937 engine(0);
  for (size_t i = 0; i < n; ++i) {
    string_data[i] = RandomString(engine, str_len);
    data[i] = {string_data[i].data(), string_data[i].size()};
  }

  for (auto _ : state) {
    std::vector<StringPtr> working_copy = data;
    std::sort(working_copy.begin(), working_copy.end(),
              [](const StringPtr& a, const StringPtr& b) {
                return std::string_view(a.data, a.size) <
                       std::string_view(b.data, b.size);
              });
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * n));
}
BENCHMARK(BM_RadixSortStdStringPtr)->Apply(RadixSortMsdArgs);

}  // namespace
