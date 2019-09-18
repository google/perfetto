// Copyright (C) 2019 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <random>

#include <benchmark/benchmark.h>

#include "src/trace_processor/db/bit_vector.h"

static void BM_BitVectorAppend(benchmark::State& state) {
  static constexpr uint32_t kPoolSize = 1024 * 1024;
  std::vector<bool> bit_pool(kPoolSize);

  static constexpr uint32_t kRandomSeed = 42;
  std::minstd_rand0 rnd_engine(kRandomSeed);
  for (uint32_t i = 0; i < kPoolSize; ++i) {
    bit_pool[i] = rnd_engine() % 2;
  }

  perfetto::trace_processor::BitVector bv;
  uint32_t pool_idx = 0;
  for (auto _ : state) {
    bv.Append(bit_pool[pool_idx]);
    pool_idx = (pool_idx + 1) % kPoolSize;
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_BitVectorAppend);

static void BM_BitVectorSet(benchmark::State& state) {
  static constexpr uint32_t kPoolSize = 1024 * 1024;
  std::vector<bool> bit_pool(kPoolSize);
  std::vector<uint32_t> row_pool(kPoolSize);

  static constexpr uint32_t kSize = 123456;
  perfetto::trace_processor::BitVector bv;

  static constexpr uint32_t kRandomSeed = 42;
  std::minstd_rand0 rnd_engine(kRandomSeed);
  for (uint32_t i = 0; i < kPoolSize; ++i) {
    bit_pool[i] = rnd_engine() % 2;
    row_pool[i] = rnd_engine() % kSize;
  }

  for (uint32_t i = 0; i < kSize; ++i) {
    bv.Append(rnd_engine() % 2);
  }

  uint32_t pool_idx = 0;
  for (auto _ : state) {
    bv.Set(row_pool[pool_idx], bit_pool[pool_idx]);
    pool_idx = (pool_idx + 1) % kPoolSize;
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_BitVectorSet);

static void BM_BitVectorIndexOfNthSet(benchmark::State& state) {
  static constexpr uint32_t kPoolSize = 1024 * 1024;
  std::vector<uint32_t> row_pool(kPoolSize);

  static constexpr uint32_t kSize = 123456;
  perfetto::trace_processor::BitVector bv;

  static constexpr uint32_t kRandomSeed = 42;
  std::minstd_rand0 rnd_engine(kRandomSeed);
  for (uint32_t i = 0; i < kSize; ++i) {
    bool value = rnd_engine() % 2;
    bv.Append(value);
  }

  uint32_t set_bit_count = bv.GetNumBitsSet();
  for (uint32_t i = 0; i < kPoolSize; ++i) {
    row_pool[i] = rnd_engine() % set_bit_count;
  }

  uint32_t pool_idx = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(bv.IndexOfNthSet(row_pool[pool_idx]));
    pool_idx = (pool_idx + 1) % kPoolSize;
  }
}
BENCHMARK(BM_BitVectorIndexOfNthSet);

static void BM_BitVectorGetNumBitsSet(benchmark::State& state) {
  static constexpr uint32_t kSize = 123456;
  perfetto::trace_processor::BitVector bv;
  uint32_t count = 0;

  static constexpr uint32_t kRandomSeed = 42;
  std::minstd_rand0 rnd_engine(kRandomSeed);
  for (uint32_t i = 0; i < kSize; ++i) {
    bool value = rnd_engine() % 2;
    bv.Append(value);

    if (value)
      count++;
  }

  uint32_t res = count;
  for (auto _ : state) {
    benchmark::DoNotOptimize(res &= bv.GetNumBitsSet());
  }
  PERFETTO_CHECK(res == count);
}
BENCHMARK(BM_BitVectorGetNumBitsSet);
