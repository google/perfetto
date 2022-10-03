// Copyright (C) 2022 The Android Open Source Project
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

#include "src/trace_processor/db/column_storage_overlay.h"

using perfetto::trace_processor::BitVector;
using perfetto::trace_processor::ColumnStorageOverlay;
using perfetto::trace_processor::RowMap;

namespace {

static constexpr uint32_t kPoolSize = 100000;
static constexpr uint32_t kSize = 123456;

template <typename Container>
Container CreateRange(uint32_t end) {
  static constexpr uint32_t kRandomSeed = 32;
  std::minstd_rand0 rnd_engine(kRandomSeed);

  uint32_t start = rnd_engine() % end;
  uint32_t size = rnd_engine() % (end - start);
  return Container(start, start + size);
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

template <typename Factory>
void BenchFilterInto(benchmark::State& state,
                     ColumnStorageOverlay rm,
                     Factory factory) {
  auto pool_vec = CreateIndexVector(kPoolSize, kSize);

  uint32_t pool_idx = 0;
  for (auto _ : state) {
    state.PauseTiming();
    RowMap out = factory();
    state.ResumeTiming();

    auto fn = [&pool_vec, pool_idx](uint32_t row) {
      return pool_vec[pool_idx] != 0 && (row % pool_vec[pool_idx]) != 0;
    };
    rm.FilterInto(&out, fn);
    pool_idx = (pool_idx + 1) % kPoolSize;

    benchmark::ClobberMemory();
  }
}

}  // namespace

static void BM_CSOFilterIntoRangeWithRange(benchmark::State& state) {
  ColumnStorageOverlay overlay(CreateRange<ColumnStorageOverlay>(kSize));
  uint32_t overlay_size = overlay.size();
  BenchFilterInto(state, std::move(overlay), [overlay_size]() {
    return CreateRange<RowMap>(overlay_size);
  });
}
BENCHMARK(BM_CSOFilterIntoRangeWithRange);

static void BM_CSOFilterIntoRangeWithBv(benchmark::State& state) {
  ColumnStorageOverlay overlay(CreateRange<ColumnStorageOverlay>(kSize));
  uint32_t overlay_size = overlay.size();
  BenchFilterInto(state, std::move(overlay), [overlay_size]() {
    return RowMap(CreateBitVector(overlay_size));
  });
}
BENCHMARK(BM_CSOFilterIntoRangeWithBv);

static void BM_CSOFilterIntoBvWithRange(benchmark::State& state) {
  ColumnStorageOverlay overlay(CreateBitVector(kSize));
  uint32_t overlay_size = overlay.size();
  BenchFilterInto(state, std::move(overlay), [overlay_size]() {
    return CreateRange<RowMap>(overlay_size);
  });
}
BENCHMARK(BM_CSOFilterIntoBvWithRange);

static void BM_CSOFilterIntoBvWithBv(benchmark::State& state) {
  ColumnStorageOverlay overlay(CreateBitVector(kSize));
  uint32_t overlay_size = overlay.size();
  BenchFilterInto(state, std::move(overlay), [overlay_size]() {
    return RowMap(CreateBitVector(overlay_size));
  });
}
BENCHMARK(BM_CSOFilterIntoBvWithBv);

static void BM_CSOFilterIntoIvWithRange(benchmark::State& state) {
  ColumnStorageOverlay overlay(CreateIndexVector(kSize, kSize));
  uint32_t overlay_size = overlay.size();
  BenchFilterInto(state, std::move(overlay), [overlay_size]() {
    return CreateRange<RowMap>(overlay_size);
  });
}
BENCHMARK(BM_CSOFilterIntoIvWithRange);

static void BM_CSOFilterIntoIvWithBv(benchmark::State& state) {
  ColumnStorageOverlay overlay(CreateIndexVector(kSize, kSize));
  uint32_t overlay_size = overlay.size();
  BenchFilterInto(state, std::move(overlay), [overlay_size]() {
    return RowMap(CreateBitVector(overlay_size));
  });
}
BENCHMARK(BM_CSOFilterIntoIvWithBv);
