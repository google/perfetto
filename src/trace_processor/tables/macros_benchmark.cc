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

#include "src/trace_processor/tables/macros.h"

namespace perfetto {
namespace trace_processor {
namespace {

#define PERFETTO_TP_ROOT_TEST_TABLE(NAME, PARENT, C) \
  NAME(RootTestTable, "root_table")                  \
  PERFETTO_TP_ROOT_TABLE(PARENT, C)                  \
  C(uint32_t, root_sorted, Column::Flag::kSorted)    \
  C(uint32_t, root_non_null)                         \
  C(base::Optional<uint32_t>, root_nullable)

PERFETTO_TP_TABLE(PERFETTO_TP_ROOT_TEST_TABLE);

#define PERFETTO_TP_CHILD_TABLE(NAME, PARENT, C)   \
  NAME(ChildTestTable, "child_table")              \
  PARENT(PERFETTO_TP_ROOT_TEST_TABLE, C)           \
  C(uint32_t, child_sorted, Column::Flag::kSorted) \
  C(uint32_t, child_non_null)                      \
  C(base::Optional<uint32_t>, child_nullable)

PERFETTO_TP_TABLE(PERFETTO_TP_CHILD_TABLE);

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto

using perfetto::trace_processor::ChildTestTable;
using perfetto::trace_processor::RootTestTable;
using perfetto::trace_processor::SqlValue;
using perfetto::trace_processor::StringPool;

static void BM_TableInsert(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool, nullptr);

  for (auto _ : state) {
    benchmark::DoNotOptimize(root.Insert({}));
  }
}
BENCHMARK(BM_TableInsert);

static void BM_TableIteratorChild(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool, nullptr);
  ChildTestTable child(&pool, &root);

  uint32_t size = static_cast<uint32_t>(state.range(0));
  for (uint32_t i = 0; i < size; ++i) {
    child.Insert({});
    root.Insert({});
  }

  auto it = child.IterateRows();
  for (auto _ : state) {
    for (uint32_t i = 0; i < child.GetColumnCount(); ++i) {
      benchmark::DoNotOptimize(it.Get(i));
    }
    it.Next();
    if (!it)
      it = child.IterateRows();
  }
}
BENCHMARK(BM_TableIteratorChild)
    ->RangeMultiplier(8)
    ->Range(1024, 2 * 1024 * 1024);

static void BM_TableFilterIdColumn(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool, nullptr);

  uint32_t size = static_cast<uint32_t>(state.range(0));
  for (uint32_t i = 0; i < size; ++i)
    root.Insert({});

  for (auto _ : state) {
    benchmark::DoNotOptimize(root.Filter({root.id().eq(SqlValue::Long(30))}));
  }
}
BENCHMARK(BM_TableFilterIdColumn)
    ->RangeMultiplier(8)
    ->Range(1024, 2 * 1024 * 1024);

static void BM_TableFilterRootNonNullEqMatchMany(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool, nullptr);

  uint32_t size = static_cast<uint32_t>(state.range(0));
  uint32_t partitions = size / 1024;

  constexpr uint32_t kRandomSeed = 42;
  std::minstd_rand0 rnd_engine(kRandomSeed);
  for (uint32_t i = 0; i < size; ++i) {
    RootTestTable::Row row(static_cast<uint32_t>(rnd_engine() % partitions));
    root.Insert(row);
  }

  for (auto _ : state) {
    benchmark::DoNotOptimize(
        root.Filter({root.root_non_null().eq(SqlValue::Long(0))}));
  }
}
BENCHMARK(BM_TableFilterRootNonNullEqMatchMany)
    ->RangeMultiplier(8)
    ->Range(1024, 2 * 1024 * 1024);

static void BM_TableFilterRootNullableEqMatchMany(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool, nullptr);

  uint32_t size = static_cast<uint32_t>(state.range(0));
  uint32_t partitions = size / 512;

  constexpr uint32_t kRandomSeed = 42;
  std::minstd_rand0 rnd_engine(kRandomSeed);
  for (uint32_t i = 0; i < size; ++i) {
    uint32_t value = rnd_engine() % partitions;

    RootTestTable::Row row;
    row.root_nullable = value % 2 == 0 ? perfetto::base::nullopt
                                       : perfetto::base::make_optional(value);
    root.Insert(row);
  }

  for (auto _ : state) {
    benchmark::DoNotOptimize(
        root.Filter({root.root_nullable().eq(SqlValue::Long(1))}));
  }
}
BENCHMARK(BM_TableFilterRootNullableEqMatchMany)
    ->RangeMultiplier(8)
    ->Range(1024, 2 * 1024 * 1024);

static void BM_TableFilterChildNonNullEqMatchMany(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool, nullptr);
  ChildTestTable child(&pool, &root);

  uint32_t size = static_cast<uint32_t>(state.range(0));
  uint32_t partitions = size / 1024;

  constexpr uint32_t kRandomSeed = 42;
  std::minstd_rand0 rnd_engine(kRandomSeed);
  for (uint32_t i = 0; i < size; ++i) {
    ChildTestTable::Row row;
    row.child_non_null = static_cast<uint32_t>(rnd_engine() % partitions);
    root.Insert({});
    child.Insert(row);
  }

  for (auto _ : state) {
    benchmark::DoNotOptimize(
        child.Filter({child.child_non_null().eq(SqlValue::Long(0))}));
  }
}
BENCHMARK(BM_TableFilterChildNonNullEqMatchMany)
    ->RangeMultiplier(8)
    ->Range(1024, 2 * 1024 * 1024);

static void BM_TableFilterChildNullableEqMatchMany(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool, nullptr);
  ChildTestTable child(&pool, &root);

  uint32_t size = static_cast<uint32_t>(state.range(0));
  uint32_t partitions = size / 512;

  constexpr uint32_t kRandomSeed = 42;
  std::minstd_rand0 rnd_engine(kRandomSeed);
  for (uint32_t i = 0; i < size; ++i) {
    uint32_t value = rnd_engine() % partitions;

    ChildTestTable::Row row;
    row.child_nullable = value % 2 == 0 ? perfetto::base::nullopt
                                        : perfetto::base::make_optional(value);
    root.Insert({});
    child.Insert(row);
  }

  for (auto _ : state) {
    benchmark::DoNotOptimize(
        child.Filter({child.child_nullable().eq(SqlValue::Long(1))}));
  }
}
BENCHMARK(BM_TableFilterChildNullableEqMatchMany)
    ->RangeMultiplier(8)
    ->Range(1024, 2 * 1024 * 1024);

static void BM_TableFilterChildNonNullEqMatchManyInParent(
    benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool, nullptr);
  ChildTestTable child(&pool, &root);

  uint32_t size = static_cast<uint32_t>(state.range(0));
  uint32_t partitions = size / 1024;

  constexpr uint32_t kRandomSeed = 42;
  std::minstd_rand0 rnd_engine(kRandomSeed);
  for (uint32_t i = 0; i < size; ++i) {
    ChildTestTable::Row row;
    row.root_non_null = static_cast<uint32_t>(rnd_engine() % partitions);
    root.Insert({});
    child.Insert(row);
  }

  for (auto _ : state) {
    benchmark::DoNotOptimize(
        child.Filter({child.root_non_null().eq(SqlValue::Long(0))}));
  }
}
BENCHMARK(BM_TableFilterChildNonNullEqMatchManyInParent)
    ->RangeMultiplier(8)
    ->Range(1024, 2 * 1024 * 1024);

static void BM_TableFilterChildNullableEqMatchManyInParent(
    benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool, nullptr);
  ChildTestTable child(&pool, &root);

  uint32_t size = static_cast<uint32_t>(state.range(0));
  uint32_t partitions = size / 512;

  constexpr uint32_t kRandomSeed = 42;
  std::minstd_rand0 rnd_engine(kRandomSeed);
  for (uint32_t i = 0; i < size; ++i) {
    ChildTestTable::Row row;
    row.root_nullable = static_cast<uint32_t>(rnd_engine() % partitions);
    root.Insert({});
    child.Insert(row);
  }

  for (auto _ : state) {
    benchmark::DoNotOptimize(
        child.Filter({child.root_nullable().eq(SqlValue::Long(1))}));
  }
}
BENCHMARK(BM_TableFilterChildNullableEqMatchManyInParent)
    ->RangeMultiplier(8)
    ->Range(1024, 2 * 1024 * 1024);

static void BM_TableFilterParentSortedEq(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool, nullptr);

  uint32_t size = static_cast<uint32_t>(state.range(0));

  constexpr uint32_t kRandomSeed = 42;
  std::minstd_rand0 rnd_engine(kRandomSeed);
  for (uint32_t i = 0; i < size; ++i) {
    RootTestTable::Row row;
    row.root_sorted = i * 2;
    root.Insert(row);
  }

  for (auto _ : state) {
    benchmark::DoNotOptimize(
        root.Filter({root.root_sorted().eq(SqlValue::Long(22))}));
  }
}
BENCHMARK(BM_TableFilterParentSortedEq)
    ->RangeMultiplier(8)
    ->Range(1024, 2 * 1024 * 1024);

static void BM_TableFilterChildSortedEq(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool, nullptr);
  ChildTestTable child(&pool, &root);

  uint32_t size = static_cast<uint32_t>(state.range(0));

  constexpr uint32_t kRandomSeed = 42;
  std::minstd_rand0 rnd_engine(kRandomSeed);
  for (uint32_t i = 0; i < size; ++i) {
    ChildTestTable::Row row;
    row.child_sorted = i * 2;
    root.Insert({});
    child.Insert(row);
  }

  for (auto _ : state) {
    benchmark::DoNotOptimize(
        child.Filter({child.child_sorted().eq(SqlValue::Long(22))}));
  }
}
BENCHMARK(BM_TableFilterChildSortedEq)
    ->RangeMultiplier(8)
    ->Range(1024, 2 * 1024 * 1024);

static void BM_TableFilterChildSortedEqInParent(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool, nullptr);
  ChildTestTable child(&pool, &root);

  uint32_t size = static_cast<uint32_t>(state.range(0));

  constexpr uint32_t kRandomSeed = 42;
  std::minstd_rand0 rnd_engine(kRandomSeed);
  for (uint32_t i = 0; i < size; ++i) {
    RootTestTable::Row root_row;
    root_row.root_sorted = i * 4;
    root.Insert({});

    ChildTestTable::Row row;
    row.root_sorted = i * 4 + 2;
    child.Insert(row);
  }

  for (auto _ : state) {
    benchmark::DoNotOptimize(
        child.Filter({child.root_sorted().eq(SqlValue::Long(22))}));
  }
}
BENCHMARK(BM_TableFilterChildSortedEqInParent)
    ->RangeMultiplier(8)
    ->Range(1024, 2 * 1024 * 1024);
