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

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <random>

#include <benchmark/benchmark.h>

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/row_map.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/db/table.h"
#include "src/trace_processor/tables/py_tables_benchmark_py.h"

namespace perfetto::trace_processor::tables {

RootTestTable::~RootTestTable() = default;
ChildTestTable::~ChildTestTable() = default;

}  // namespace perfetto::trace_processor::tables

namespace {

bool IsBenchmarkFunctionalOnly() {
  return getenv("BENCHMARK_FUNCTIONAL_TEST_ONLY") != nullptr;
}

void TableFilterArgs(benchmark::internal::Benchmark* b) {
  if (IsBenchmarkFunctionalOnly()) {
    b->Arg(1024);
  } else {
    b->Arg(2ull * 1024 * 1024);
  }
}

void TableSortArgs(benchmark::internal::Benchmark* b) {
  if (IsBenchmarkFunctionalOnly()) {
    b->Arg(64);
  } else {
    b->Arg(256ull * 1024);
  }
}

}  // namespace

using perfetto::trace_processor::Query;
using perfetto::trace_processor::RowMap;
using perfetto::trace_processor::SqlValue;
using perfetto::trace_processor::StringPool;
using perfetto::trace_processor::Table;
using perfetto::trace_processor::tables::ChildTestTable;
using perfetto::trace_processor::tables::RootTestTable;

static void BM_TableInsert(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool);

  for (auto _ : state) {
    benchmark::DoNotOptimize(root.Insert({}));
  }
}
BENCHMARK(BM_TableInsert);

static void BM_TableIteratorChild(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool);
  ChildTestTable child(&pool, &root);

  uint32_t size = static_cast<uint32_t>(state.range(0));
  for (uint32_t i = 0; i < size; ++i) {
    child.Insert({});
    root.Insert({});
  }

  auto it = static_cast<Table&>(child).IterateRows();
  for (auto _ : state) {
    for (uint32_t i = 0; i < child.columns().size(); ++i) {
      benchmark::DoNotOptimize(it.Get(i));
    }
    if (!++it)
      it = static_cast<Table&>(child).IterateRows();
  }
}
BENCHMARK(BM_TableIteratorChild)->Apply(TableFilterArgs);

static void BM_TableFilterRootId(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool);
  Query q;
  q.constraints = {root.id().eq(30)};

  uint32_t size = static_cast<uint32_t>(state.range(0));
  for (uint32_t i = 0; i < size; ++i)
    root.Insert({});

  for (auto _ : state) {
    benchmark::DoNotOptimize(root.FilterToIterator(q));
  }
}
BENCHMARK(BM_TableFilterRootId)->Apply(TableFilterArgs);

static void BM_TableFilterRootIdAndOther(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool);
  Query q;
  q.constraints = {root.id().eq(root.row_count() - 1),
                   root.root_non_null().gt(100)};

  uint32_t size = static_cast<uint32_t>(state.range(0));

  for (uint32_t i = 0; i < size; ++i) {
    RootTestTable::Row root_row;
    root_row.root_non_null = i * 4;
    root.Insert(root_row);
  }

  for (auto _ : state) {
    benchmark::DoNotOptimize(root.FilterToIterator(q));
  }
}
BENCHMARK(BM_TableFilterRootIdAndOther)->Apply(TableFilterArgs);

static void BM_TableFilterChildId(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool);
  ChildTestTable child(&pool, &root);
  Query q;
  q.constraints = {child.id().eq(30)};

  uint32_t size = static_cast<uint32_t>(state.range(0));
  for (uint32_t i = 0; i < size; ++i) {
    root.Insert({});
    child.Insert({});
  }

  for (auto _ : state) {
    benchmark::DoNotOptimize(child.FilterToIterator(q));
  }
}
BENCHMARK(BM_TableFilterChildId)->Apply(TableFilterArgs);

static void BM_TableFilterChildIdAndSortedInRoot(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool);
  ChildTestTable child(&pool, &root);
  Query q;
  q.constraints = {child.id().eq(30), child.root_sorted().gt(1024)};

  uint32_t size = static_cast<uint32_t>(state.range(0));
  for (uint32_t i = 0; i < size; ++i) {
    RootTestTable::Row root_row;
    root_row.root_sorted = i * 2;
    root.Insert(root_row);

    ChildTestTable::Row child_row;
    child_row.root_sorted = i * 2 + 1;
    child.Insert(child_row);
  }

  for (auto _ : state) {
    benchmark::DoNotOptimize(child.FilterToIterator(q));
  }
}
BENCHMARK(BM_TableFilterChildIdAndSortedInRoot)->Apply(TableFilterArgs);

static void BM_TableFilterRootNonNullEqMatchMany(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool);
  Query q;
  q.constraints = {root.root_non_null().eq(0)};

  auto size = static_cast<uint32_t>(state.range(0));
  uint32_t partitions = size / 1024;

  std::minstd_rand0 rnd_engine;
  for (uint32_t i = 0; i < size; ++i) {
    RootTestTable::Row row(static_cast<uint32_t>(rnd_engine() % partitions));
    root.Insert(row);
  }

  for (auto _ : state) {
    benchmark::DoNotOptimize(root.FilterToIterator(q));
  }
}
BENCHMARK(BM_TableFilterRootNonNullEqMatchMany)->Apply(TableFilterArgs);

static void BM_TableFilterRootMultipleNonNull(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool);
  Query q;
  q.constraints = {root.root_non_null().lt(4), root.root_non_null_2().lt(10)};

  auto size = static_cast<uint32_t>(state.range(0));
  uint32_t partitions = size / 512;

  std::minstd_rand0 rnd_engine;
  for (uint32_t i = 0; i < size; ++i) {
    RootTestTable::Row row;
    row.root_non_null = rnd_engine() % partitions;
    row.root_non_null_2 = rnd_engine() % partitions;
    root.Insert(row);
  }

  for (auto _ : state) {
    benchmark::DoNotOptimize(root.FilterToIterator(q));
  }
}
BENCHMARK(BM_TableFilterRootMultipleNonNull)->Apply(TableFilterArgs);

static void BM_TableFilterRootNullableEqMatchMany(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool);
  Query q;
  q.constraints = {root.root_nullable().eq(1)};

  auto size = static_cast<uint32_t>(state.range(0));
  uint32_t partitions = size / 512;

  std::minstd_rand0 rnd_engine;
  for (uint32_t i = 0; i < size; ++i) {
    uint32_t value = rnd_engine() % partitions;

    RootTestTable::Row row;
    row.root_nullable =
        value % 2 == 0 ? std::nullopt : std::make_optional(value);
    root.Insert(row);
  }

  for (auto _ : state) {
    benchmark::DoNotOptimize(root.FilterToIterator(q));
  }
}
BENCHMARK(BM_TableFilterRootNullableEqMatchMany)->Apply(TableFilterArgs);

static void BM_TableFilterChildNonNullEqMatchMany(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool);
  ChildTestTable child(&pool, &root);
  Query q;
  q.constraints = {child.child_non_null().eq(0)};

  auto size = static_cast<uint32_t>(state.range(0));
  uint32_t partitions = size / 1024;

  std::minstd_rand0 rnd_engine;
  for (uint32_t i = 0; i < size; ++i) {
    ChildTestTable::Row row;
    row.child_non_null = static_cast<uint32_t>(rnd_engine() % partitions);
    root.Insert({});
    child.Insert(row);
  }

  for (auto _ : state) {
    benchmark::DoNotOptimize(child.FilterToIterator(q));
  }
}
BENCHMARK(BM_TableFilterChildNonNullEqMatchMany)->Apply(TableFilterArgs);

static void BM_TableFilterChildNullableEqMatchMany(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool);
  ChildTestTable child(&pool, &root);
  Query q;
  q.constraints = {child.child_nullable().eq(1)};

  auto size = static_cast<uint32_t>(state.range(0));
  uint32_t partitions = size / 512;

  std::minstd_rand0 rnd_engine;
  for (uint32_t i = 0; i < size; ++i) {
    uint32_t value = rnd_engine() % partitions;

    ChildTestTable::Row row;
    row.child_nullable =
        value % 2 == 0 ? std::nullopt : std::make_optional(value);
    root.Insert({});
    child.Insert(row);
  }

  for (auto _ : state) {
    benchmark::DoNotOptimize(child.FilterToIterator(q));
  }
}
BENCHMARK(BM_TableFilterChildNullableEqMatchMany)->Apply(TableFilterArgs);

static void BM_TableFilterChildNonNullEqMatchManyInParent(
    benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool);
  ChildTestTable child(&pool, &root);
  Query q;
  q.constraints = {child.root_non_null().eq(0)};

  auto size = static_cast<uint32_t>(state.range(0));
  uint32_t partitions = size / 1024;

  std::minstd_rand0 rnd_engine;
  for (uint32_t i = 0; i < size; ++i) {
    ChildTestTable::Row row;
    row.root_non_null = static_cast<uint32_t>(rnd_engine() % partitions);
    root.Insert({});
    child.Insert(row);
  }

  for (auto _ : state) {
    benchmark::DoNotOptimize(child.FilterToIterator(q));
  }
}
BENCHMARK(BM_TableFilterChildNonNullEqMatchManyInParent)
    ->Apply(TableFilterArgs);

static void BM_TableFilterChildNullableEqMatchManyInParent(
    benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool);
  ChildTestTable child(&pool, &root);

  auto size = static_cast<uint32_t>(state.range(0));
  uint32_t partitions = size / 512;

  std::minstd_rand0 rnd_engine;
  for (uint32_t i = 0; i < size; ++i) {
    ChildTestTable::Row row;
    row.root_nullable = static_cast<uint32_t>(rnd_engine() % partitions);
    root.Insert({});
    child.Insert(row);
  }

  Query q;
  q.constraints = {child.root_nullable().eq(1)};
  for (auto _ : state) {
    benchmark::DoNotOptimize(child.FilterToIterator(q));
  }
}
BENCHMARK(BM_TableFilterChildNullableEqMatchManyInParent)
    ->Apply(TableFilterArgs);

static void BM_TableFilterParentSortedEq(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool);

  auto size = static_cast<uint32_t>(state.range(0));

  for (uint32_t i = 0; i < size; ++i) {
    RootTestTable::Row row;
    row.root_sorted = i * 2;
    root.Insert(row);
  }

  Query q;
  q.constraints = {root.root_sorted().eq(22)};
  for (auto _ : state) {
    benchmark::DoNotOptimize(root.FilterToIterator(q));
  }
}
BENCHMARK(BM_TableFilterParentSortedEq)->Apply(TableFilterArgs);

static void BM_TableFilterParentSortedAndOther(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool);

  auto size = static_cast<uint32_t>(state.range(0));

  for (uint32_t i = 0; i < size; ++i) {
    // Group the rows into rows of 10. This emulates the behaviour of e.g.
    // args.
    RootTestTable::Row row;
    row.root_sorted = (i / 10) * 10;
    row.root_non_null = i;
    root.Insert(row);
  }

  // We choose to search for the last group as if there is O(n^2), it will
  // be more easily visible.
  uint32_t last_group = ((size - 1) / 10) * 10;
  Query q;
  q.constraints = {root.root_sorted().eq(last_group),
                   root.root_non_null().eq(size - 1)};
  for (auto _ : state) {
    benchmark::DoNotOptimize(root.FilterToIterator(q));
  }
}
BENCHMARK(BM_TableFilterParentSortedAndOther)->Apply(TableFilterArgs);

static void BM_TableFilterChildSortedEq(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool);
  ChildTestTable child(&pool, &root);

  auto size = static_cast<uint32_t>(state.range(0));

  for (uint32_t i = 0; i < size; ++i) {
    ChildTestTable::Row row;
    row.child_sorted = i * 2;
    root.Insert({});
    child.Insert(row);
  }

  Query q;
  q.constraints = {child.child_sorted().eq(22)};
  for (auto _ : state) {
    benchmark::DoNotOptimize(child.FilterToIterator(q));
  }
}
BENCHMARK(BM_TableFilterChildSortedEq)->Apply(TableFilterArgs);

static void BM_TableFilterChildSortedEqInParent(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool);
  ChildTestTable child(&pool, &root);

  auto size = static_cast<uint32_t>(state.range(0));

  for (uint32_t i = 0; i < size; ++i) {
    RootTestTable::Row root_row;
    root_row.root_sorted = i * 4;
    root.Insert(root_row);

    ChildTestTable::Row row;
    row.root_sorted = i * 4 + 2;
    child.Insert(row);
  }

  Query q;
  q.constraints = {child.root_sorted().eq(22)};
  for (auto _ : state) {
    benchmark::DoNotOptimize(child.FilterToIterator(q));
  }
}
BENCHMARK(BM_TableFilterChildSortedEqInParent)->Apply(TableFilterArgs);

static void BM_TableSortRootNonNull(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool);

  auto size = static_cast<uint32_t>(state.range(0));

  std::minstd_rand0 rnd_engine;
  for (uint32_t i = 0; i < size; ++i) {
    const uint32_t root_value = static_cast<uint32_t>(rnd_engine());

    RootTestTable::Row row;
    row.root_non_null = root_value;
    root.Insert(row);
  }

  Query q;
  q.orders = {root.root_non_null().ascending()};
  for (auto _ : state) {
    benchmark::DoNotOptimize(root.FilterToIterator(q));
  }
}
BENCHMARK(BM_TableSortRootNonNull)->Apply(TableSortArgs);

static void BM_TableSortRootNullable(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool);

  auto size = static_cast<uint32_t>(state.range(0));

  std::minstd_rand0 rnd_engine;
  for (uint32_t i = 0; i < size; ++i) {
    const uint32_t root_value = static_cast<uint32_t>(rnd_engine());

    RootTestTable::Row row;
    row.root_nullable =
        root_value % 2 == 0 ? std::nullopt : std::make_optional(root_value);
    root.Insert(row);
  }

  Query q;
  q.orders = {root.root_nullable().ascending()};
  for (auto _ : state) {
    benchmark::DoNotOptimize(root.FilterToIterator(q));
  }
}
BENCHMARK(BM_TableSortRootNullable)->Apply(TableSortArgs);

static void BM_TableSortChildNonNullInParent(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool);
  ChildTestTable child(&pool, &root);

  auto size = static_cast<uint32_t>(state.range(0));

  std::minstd_rand0 rnd_engine;
  for (uint32_t i = 0; i < size; ++i) {
    const uint32_t root_value = static_cast<uint32_t>(rnd_engine());

    RootTestTable::Row root_row;
    root_row.root_non_null = root_value;
    root.Insert(root_row);

    const uint32_t child_value = static_cast<uint32_t>(rnd_engine());

    ChildTestTable::Row child_row;
    child_row.root_non_null = child_value;
    child.Insert(child_row);
  }

  Query q;
  q.orders = {child.root_non_null().ascending()};
  for (auto _ : state) {
    benchmark::DoNotOptimize(child.FilterToIterator(q));
  }
}
BENCHMARK(BM_TableSortChildNonNullInParent)->Apply(TableSortArgs);

static void BM_TableSortChildNullableInParent(benchmark::State& state) {
  StringPool pool;
  RootTestTable root(&pool);
  ChildTestTable child(&pool, &root);

  auto size = static_cast<uint32_t>(state.range(0));

  std::minstd_rand0 rnd_engine;
  for (uint32_t i = 0; i < size; ++i) {
    const auto root_value = static_cast<uint32_t>(rnd_engine());

    RootTestTable::Row root_row;
    root_row.root_nullable =
        root_value % 2 == 0 ? std::nullopt : std::make_optional(root_value);
    root.Insert(root_row);

    const auto child_value = static_cast<uint32_t>(rnd_engine());

    ChildTestTable::Row child_row;
    child_row.root_nullable =
        child_value % 2 == 0 ? std::nullopt : std::make_optional(child_value);
    child.Insert(child_row);
  }

  Query q;
  q.orders = {child.root_nullable().ascending()};
  for (auto _ : state) {
    benchmark::DoNotOptimize(child.FilterToIterator(q));
  }
}
BENCHMARK(BM_TableSortChildNullableInParent)->Apply(TableSortArgs);
