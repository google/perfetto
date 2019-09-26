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

#include "src/trace_processor/db/row_map.h"

using perfetto::trace_processor::BitVector;
using perfetto::trace_processor::RowMap;

namespace {

static constexpr uint32_t kPoolSize = 100000;
static constexpr uint32_t kSize = 123456;

std::vector<uint32_t> CreateRandomIndexVector(uint32_t size, uint32_t mod) {
  static constexpr uint32_t kRandomSeed = 476;
  std::minstd_rand0 rnd_engine(kRandomSeed);
  std::vector<uint32_t> rows(size);
  for (uint32_t i = 0; i < size; ++i) {
    rows[i] = rnd_engine() % mod;
  }
  return rows;
}

BitVector CreateRandomBitVector(uint32_t size) {
  static constexpr uint32_t kRandomSeed = 42;
  std::minstd_rand0 rnd_engine(kRandomSeed);
  BitVector bv;
  for (uint32_t i = 0; i < size; ++i) {
    if (rnd_engine() % 2) {
      bv.AppendTrue();
    } else {
      bv.AppendFalse();
    }
  }
  return bv;
}

}  // namespace

static void BM_RowMapBitVectorGet(benchmark::State& state) {
  RowMap rm(CreateRandomBitVector(kSize));
  auto pool_vec = CreateRandomIndexVector(kPoolSize, rm.size());

  uint32_t pool_idx = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(rm.Get(pool_vec[pool_idx]));
    pool_idx = (pool_idx + 1) % kPoolSize;
  }
}
BENCHMARK(BM_RowMapBitVectorGet);

static void BM_RowMapIndexVectorGet(benchmark::State& state) {
  RowMap rm(CreateRandomIndexVector(kSize, kSize));
  auto pool_vec = CreateRandomIndexVector(kPoolSize, kSize);

  uint32_t pool_idx = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(rm.Get(pool_vec[pool_idx]));
    pool_idx = (pool_idx + 1) % kPoolSize;
  }
}
BENCHMARK(BM_RowMapIndexVectorGet);

// TODO(lalitm): add benchmarks for IndexOf after BitVector is made faster.
// We can't add them right now because they are just too slow to run.

static void BM_RowMapBitVectorAdd(benchmark::State& state) {
  auto pool_vec = CreateRandomIndexVector(kPoolSize, kSize);

  RowMap rm(BitVector{});
  uint32_t pool_idx = 0;
  for (auto _ : state) {
    rm.Add(pool_vec[pool_idx]);
    pool_idx = (pool_idx + 1) % kPoolSize;
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_RowMapBitVectorAdd);

static void BM_RowMapIndexVectorAdd(benchmark::State& state) {
  auto pool_vec = CreateRandomIndexVector(kPoolSize, kSize);

  RowMap rm(std::vector<uint32_t>{});
  uint32_t pool_idx = 0;
  for (auto _ : state) {
    rm.Add(pool_vec[pool_idx]);
    pool_idx = (pool_idx + 1) % kPoolSize;
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_RowMapIndexVectorAdd);

static void BM_RowMapBvSelectBv(benchmark::State& state) {
  RowMap rm(CreateRandomBitVector(kSize));
  RowMap selector(CreateRandomBitVector(rm.size()));

  for (auto _ : state) {
    benchmark::DoNotOptimize(rm.SelectRows(selector));
  }
}
BENCHMARK(BM_RowMapBvSelectBv);

// TODO(lalitm): add benchmarks for BvSelectIv after BitVector is made faster.
// We can't add them right now because they are just too slow to run.

static void BM_RowMapIvSelectBv(benchmark::State& state) {
  RowMap rm(CreateRandomIndexVector(kSize, kSize));
  RowMap selector(CreateRandomBitVector(rm.size()));

  for (auto _ : state) {
    benchmark::DoNotOptimize(rm.SelectRows(selector));
  }
}
BENCHMARK(BM_RowMapIvSelectBv);

static void BM_RowMapIvSelectIv(benchmark::State& state) {
  RowMap rm(CreateRandomIndexVector(kSize, kSize));
  RowMap selector(CreateRandomIndexVector(rm.size(), rm.size()));

  for (auto _ : state) {
    benchmark::DoNotOptimize(rm.SelectRows(selector));
  }
}
BENCHMARK(BM_RowMapIvSelectIv);

static void BM_RowMapBvSelectSingleRow(benchmark::State& state) {
  // This benchmark tests the performance of selecting just a single
  // row of a RowMap. We specially test this case as it occurs on every join
  // based on id originating from SQLite; nested subqueries will be performed
  // on the id column and will select just a single row.
  RowMap rm(CreateRandomBitVector(kSize));

  static constexpr uint32_t kRandomSeed = 123;
  std::minstd_rand0 rnd_engine(kRandomSeed);
  BitVector bv(rm.size(), false);
  bv.Set(rnd_engine() % bv.size());
  RowMap selector(std::move(bv));

  for (auto _ : state) {
    benchmark::DoNotOptimize(rm.SelectRows(selector));
  }
}
BENCHMARK(BM_RowMapBvSelectSingleRow);
