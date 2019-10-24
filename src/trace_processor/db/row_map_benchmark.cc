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

RowMap CreateRange(uint32_t end) {
  static constexpr uint32_t kRandomSeed = 32;
  std::minstd_rand0 rnd_engine(kRandomSeed);

  uint32_t start = rnd_engine() % end;
  uint32_t size = rnd_engine() % (end - start);
  return RowMap(start, start + size);
}

std::vector<uint32_t> CreateIndexVector(uint32_t size, uint32_t mod) {
  static constexpr uint32_t kRandomSeed = 476;
  std::minstd_rand0 rnd_engine(kRandomSeed);
  std::vector<uint32_t> rows(size);
  for (uint32_t i = 0; i < size; ++i) {
    rows[i] = rnd_engine() % mod;
  }
  return rows;
}

BitVector CreateBitVector(uint32_t size) {
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

void BenchRowMapGet(benchmark::State& state, RowMap rm) {
  auto pool_vec = CreateIndexVector(kPoolSize, rm.size());

  uint32_t pool_idx = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(rm.Get(pool_vec[pool_idx]));
    pool_idx = (pool_idx + 1) % kPoolSize;
  }
}

template <typename Factory>
void BenchRowMapAddToEmpty(benchmark::State& state, Factory factory) {
  auto pool_vec = CreateIndexVector(kPoolSize, kSize);

  uint32_t pool_idx = 0;
  for (auto _ : state) {
    RowMap rm = factory();

    rm.Add(pool_vec[pool_idx]);
    pool_idx = (pool_idx + 1) % kPoolSize;

    benchmark::ClobberMemory();
  }
}

void BenchRowMapSelect(benchmark::State& state,
                       RowMap rm,
                       const RowMap& selector) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(rm.SelectRows(selector));
  }
}

template <typename Factory>
void BenchRowMapRemoveIf(benchmark::State& state, Factory factory) {
  auto pool_vec = CreateIndexVector(kPoolSize, kSize);

  uint32_t pool_idx = 0;
  for (auto _ : state) {
    state.PauseTiming();
    RowMap rm = factory();
    state.ResumeTiming();

    auto fn = [&pool_vec, pool_idx](uint32_t row) {
      return (row % pool_vec[pool_idx]) != 0;
    };
    rm.RemoveIf(fn);
    pool_idx = (pool_idx + 1) % kPoolSize;

    benchmark::ClobberMemory();
  }
}

}  // namespace

static void BM_RowMapRangeGet(benchmark::State& state) {
  BenchRowMapGet(state, RowMap(CreateRange(kSize)));
}
BENCHMARK(BM_RowMapRangeGet);

static void BM_RowMapBvGet(benchmark::State& state) {
  BenchRowMapGet(state, RowMap(CreateBitVector(kSize)));
}
BENCHMARK(BM_RowMapBvGet);

static void BM_RowMapIvGet(benchmark::State& state) {
  BenchRowMapGet(state, RowMap(CreateIndexVector(kSize, kSize)));
}
BENCHMARK(BM_RowMapIvGet);

// TODO(lalitm): add benchmarks for IndexOf after BitVector is made faster.
// We can't add them right now because they are just too slow to run.

static void BM_RowMapRangeAddToEmpty(benchmark::State& state) {
  BenchRowMapAddToEmpty(state, []() { return RowMap(0, 0); });
}
BENCHMARK(BM_RowMapRangeAddToEmpty);

static void BM_RowMapBvAddToEmpty(benchmark::State& state) {
  BenchRowMapAddToEmpty(state, []() { return RowMap(BitVector{}); });
}
BENCHMARK(BM_RowMapBvAddToEmpty);

static void BM_RowMapIvAddToEmpty(benchmark::State& state) {
  BenchRowMapAddToEmpty(state,
                        []() { return RowMap(std::vector<uint32_t>{}); });
}
BENCHMARK(BM_RowMapIvAddToEmpty);

static void BM_RowMapSelectRangeWithRange(benchmark::State& state) {
  RowMap rm(CreateRange(kSize));
  RowMap selector(CreateRange(rm.size()));
  BenchRowMapSelect(state, std::move(rm), std::move(selector));
}
BENCHMARK(BM_RowMapSelectRangeWithRange);

static void BM_RowMapSelectRangeWithBv(benchmark::State& state) {
  RowMap rm(CreateRange(kSize));
  RowMap selector(CreateBitVector(rm.size()));
  BenchRowMapSelect(state, std::move(rm), std::move(selector));
}
BENCHMARK(BM_RowMapSelectRangeWithBv);

static void BM_RowMapSelectRangeWithIv(benchmark::State& state) {
  RowMap rm(CreateRange(kSize));
  RowMap selector(CreateIndexVector(rm.size(), rm.size()));
  BenchRowMapSelect(state, std::move(rm), std::move(selector));
}
BENCHMARK(BM_RowMapSelectRangeWithIv);

static void BM_RowMapSelectBvWithRange(benchmark::State& state) {
  RowMap rm(CreateBitVector(kSize));
  RowMap selector(CreateRange(rm.size()));
  BenchRowMapSelect(state, std::move(rm), std::move(selector));
}
BENCHMARK(BM_RowMapSelectBvWithRange);

static void BM_RowMapSelectBvWithBv(benchmark::State& state) {
  RowMap rm(CreateBitVector(kSize));
  RowMap selector(CreateBitVector(rm.size()));
  BenchRowMapSelect(state, std::move(rm), std::move(selector));
}
BENCHMARK(BM_RowMapSelectBvWithBv);

static void BM_RowMapSelectBvWithIv(benchmark::State& state) {
  RowMap rm(CreateBitVector(kSize));
  RowMap selector(CreateIndexVector(rm.size(), rm.size()));
  BenchRowMapSelect(state, std::move(rm), std::move(selector));
}
BENCHMARK(BM_RowMapSelectBvWithIv);

static void BM_RowMapSelectIvWithRange(benchmark::State& state) {
  RowMap rm(CreateIndexVector(kSize, kSize));
  RowMap selector(CreateRange(rm.size()));
  BenchRowMapSelect(state, std::move(rm), std::move(selector));
}
BENCHMARK(BM_RowMapSelectIvWithRange);

static void BM_RowMapSelectIvWithBv(benchmark::State& state) {
  RowMap rm(CreateIndexVector(kSize, kSize));
  RowMap selector(CreateBitVector(rm.size()));
  BenchRowMapSelect(state, std::move(rm), std::move(selector));
}
BENCHMARK(BM_RowMapSelectIvWithBv);

static void BM_RowMapSelectIvWithIv(benchmark::State& state) {
  RowMap rm(CreateIndexVector(kSize, kSize));
  RowMap selector(CreateIndexVector(rm.size(), rm.size()));
  BenchRowMapSelect(state, std::move(rm), std::move(selector));
}
BENCHMARK(BM_RowMapSelectIvWithIv);

static void BM_RowMapRangeRemoveIf(benchmark::State& state) {
  BenchRowMapRemoveIf(state, []() { return RowMap(CreateRange(kSize)); });
}
BENCHMARK(BM_RowMapRangeRemoveIf);

static void BM_RowMapBvRemoveIf(benchmark::State& state) {
  BenchRowMapRemoveIf(state, []() { return RowMap(CreateBitVector(kSize)); });
}
BENCHMARK(BM_RowMapBvRemoveIf);
