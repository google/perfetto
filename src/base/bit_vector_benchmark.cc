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

#include "perfetto/ext/base/bit_vector.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include <benchmark/benchmark.h>

namespace perfetto {
namespace base {
namespace {

// Visit distinct indices across the initialized range.
// Large ranges use 8192 accesses per batch, so index traversal stays bounded.
std::vector<size_t> MakeIndices(size_t num_bits, bool shuffle) {
  const size_t count = std::min(num_bits, size_t{8192});
  std::vector<size_t> indices(count);
  std::minstd_rand rng(42);
  const size_t stride = num_bits / count;
  // Vary offsets within each interval to avoid power-of-two cache conflicts.
  for (size_t i = 0; i < count; ++i)
    indices[i] = i * stride + static_cast<size_t>(rng()) % stride;
  if (shuffle)
    std::shuffle(indices.begin(), indices.end(), rng);
  return indices;
}

void AccessCounters(benchmark::State& state, size_t operations) {
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(operations));
  state.counters["operations"] = static_cast<double>(operations);
}

void BM_BaseBitVector_IsSet(benchmark::State& state) {
  const auto num_bits = static_cast<size_t>(state.range(0));
  auto indices = MakeIndices(num_bits, state.range(1) != 0);
  BitVector bits;
  bits.TestAndSet(num_bits - 1);
  bits.ClearAll();
  for (size_t j = 0; j < indices.size(); j += 2)
    bits.TestAndSet(indices[j]);
  benchmark::DoNotOptimize(bits);
  for (auto _ : state) {
    size_t found = 0;
    for (size_t i : indices)
      found += bits.IsSet(i);
    benchmark::DoNotOptimize(found);
    benchmark::ClobberMemory();
  }
  AccessCounters(state, indices.size());
}
BENCHMARK(BM_BaseBitVector_IsSet)
    ->ArgsProduct({{64, 4096, 1 << 18, 1 << 26}, {0, 1}})
    ->ArgNames({"bits", "shuffled"});

void BM_BaseBitVector_AlreadySet(benchmark::State& state) {
  const auto num_bits = static_cast<size_t>(state.range(0));
  auto indices = MakeIndices(num_bits, state.range(1) != 0);
  BitVector bits;
  bits.TestAndSet(num_bits - 1);
  for (size_t i : indices)
    bits.TestAndSet(i);
  benchmark::DoNotOptimize(bits);
  for (auto _ : state) {
    size_t found = 0;
    for (size_t i : indices)
      found += bits.TestAndSet(i);
    benchmark::DoNotOptimize(found);
    benchmark::ClobberMemory();
  }
  AccessCounters(state, indices.size());
}
BENCHMARK(BM_BaseBitVector_AlreadySet)
    ->ArgsProduct({{64, 4096, 1 << 18, 1 << 26}, {0, 1}})
    ->ArgNames({"bits", "shuffled"});

void BM_BaseBitVector_AlreadyClear(benchmark::State& state) {
  const auto num_bits = static_cast<size_t>(state.range(0));
  auto indices = MakeIndices(num_bits, state.range(1) != 0);
  BitVector bits;
  bits.TestAndSet(num_bits - 1);
  bits.ClearAll();
  benchmark::DoNotOptimize(bits);
  for (auto _ : state) {
    for (size_t i : indices)
      bits.Clear(i);
    benchmark::ClobberMemory();
  }
  AccessCounters(state, indices.size());
}
BENCHMARK(BM_BaseBitVector_AlreadyClear)
    ->ArgsProduct({{64, 4096, 1 << 18, 1 << 26}, {0, 1}})
    ->ArgNames({"bits", "shuffled"});

// Each batch sets unset bits and then clears those same bits.
// The barrier keeps the stores observable between the two passes.
// Report per bit operation, so one set and one clear count as two operations.
void BM_BaseBitVector_SetClear(benchmark::State& state) {
  const auto num_bits = static_cast<size_t>(state.range(0));
  auto indices = MakeIndices(num_bits, state.range(1) != 0);
  BitVector bits;
  bits.TestAndSet(num_bits - 1);
  bits.ClearAll();
  benchmark::DoNotOptimize(bits);
  for (auto _ : state) {
    size_t found = 0;
    for (size_t i : indices)
      found += bits.TestAndSet(i);
    benchmark::DoNotOptimize(found);
    benchmark::ClobberMemory();
    for (size_t i : indices)
      bits.Clear(i);
    benchmark::ClobberMemory();
  }
  AccessCounters(state, indices.size() * 2);
}
BENCHMARK(BM_BaseBitVector_SetClear)
    ->ArgsProduct({{64, 4096, 1 << 18, 1 << 26}, {0, 1}})
    ->ArgNames({"bits", "shuffled"});

void BM_BaseBitVector_ClearAll(benchmark::State& state) {
  BitVector bits;
  if (state.range(0))
    bits.TestAndSet(static_cast<size_t>(state.range(0)) - 1);
  benchmark::DoNotOptimize(bits);
  for (auto _ : state) {
    bits.ClearAll();
    benchmark::ClobberMemory();
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(bits.capacity() / 8));
}
BENCHMARK(BM_BaseBitVector_ClearAll)
    ->Arg(0)
    ->Arg(64)
    ->Arg(4096)
    ->Arg(1 << 18)
    ->Arg(1 << 26)
    ->ArgName("bits");

// These lifecycle benchmarks include allocation, initialization and
// destruction.
void BM_BaseBitVector_FirstSet(benchmark::State& state) {
  const auto num_bits = static_cast<size_t>(state.range(0));
  for (auto _ : state) {
    BitVector bits;
    benchmark::DoNotOptimize(bits.TestAndSet(num_bits - 1));
    benchmark::DoNotOptimize(bits);
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_BaseBitVector_FirstSet)
    ->Arg(64)
    ->Arg(4096)
    ->Arg(1 << 18)
    ->Arg(1 << 26)
    ->ArgName("bits");

void BM_BaseBitVector_GrowAtCapacity(benchmark::State& state) {
  const auto num_bits = static_cast<size_t>(state.range(0));
  for (auto _ : state) {
    BitVector bits;
    while (bits.capacity() < num_bits)
      bits.TestAndSet(bits.capacity());
    benchmark::DoNotOptimize(bits);
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_BaseBitVector_GrowAtCapacity)
    ->Arg(4096)
    ->Arg(1 << 18)
    ->Arg(1 << 26)
    ->ArgName("bits");

void BM_BaseBitVector_GrowDense(benchmark::State& state) {
  const auto num_bits = static_cast<size_t>(state.range(0));
  for (auto _ : state) {
    BitVector bits;
    size_t found = 0;
    for (size_t i = 0; i < num_bits; ++i)
      found += bits.TestAndSet(i);
    benchmark::DoNotOptimize(found);
    benchmark::DoNotOptimize(bits);
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_BaseBitVector_GrowDense)->Arg(4096)->Arg(1 << 18)->ArgName("bits");

}  // namespace
}  // namespace base
}  // namespace perfetto
