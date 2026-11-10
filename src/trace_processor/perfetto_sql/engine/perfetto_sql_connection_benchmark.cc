// Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>

#include <benchmark/benchmark.h>
#include <sqlite3.h>

#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/sqlite/sql_source.h"

namespace perfetto::trace_processor {
namespace {

// Pure dispatch overhead through PerfettoSqlConnection::Execute().
// Each iteration pushes one kRoot frame on |execution_stack_| and pops it.
// This is the workload the (removed) Execute() fast path was optimizing.
void BM_Connection_Execute_TrivialSelect(benchmark::State& state) {
  StringPool pool;
  auto conn = PerfettoSqlConnection::CreateConnectionToNewDatabase(
      &pool, /*enable_extra_checks=*/false);
  for (auto _ : state) {
    auto res = conn->Execute(SqlSource::FromExecuteQuery("SELECT 1"));
    PERFETTO_CHECK(res.ok());
  }
}
BENCHMARK(BM_Connection_Execute_TrivialSelect);

// Re-entrant Execute(): the outer body uses a CREATE PERFETTO FUNCTION
// whose body re-enters the engine via the runtime function machinery.
// Exercises |execution_stack_| at depth > 1 — the case where SmallVector
// inline storage is exceeded and a heap allocation kicks in.
void BM_Connection_Execute_NestedFunction(benchmark::State& state) {
  StringPool pool;
  auto conn = PerfettoSqlConnection::CreateConnectionToNewDatabase(
      &pool, /*enable_extra_checks=*/false);
  auto setup = conn->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO FUNCTION inner_one() RETURNS INT AS SELECT 1"));
  PERFETTO_CHECK(setup.ok());
  for (auto _ : state) {
    auto res = conn->Execute(SqlSource::FromExecuteQuery("SELECT inner_one()"));
    PERFETTO_CHECK(res.ok());
  }
}
BENCHMARK(BM_Connection_Execute_NestedFunction);

// Small statement that goes through the PerfettoSQL-extension path rather
// than the vanilla-SQLite path: catches regressions in the non-fast-path
// code. (CREATE PERFETTO TABLE is a no-op-ish after the first call when
// OR REPLACE is used.)
void BM_Connection_Execute_PerfettoSqlExtension(benchmark::State& state) {
  StringPool pool;
  auto conn = PerfettoSqlConnection::CreateConnectionToNewDatabase(
      &pool, /*enable_extra_checks=*/false);
  for (auto _ : state) {
    auto res = conn->Execute(SqlSource::FromExecuteQuery(
        "CREATE OR REPLACE PERFETTO TABLE t AS SELECT 1 AS x"));
    PERFETTO_CHECK(res.ok());
  }
}
BENCHMARK(BM_Connection_Execute_PerfettoSqlExtension);

// Measures the per-cell SQLite adapter over flat numeric, nullable and string
// columns. The large scan amortizes per-statement table creation and planning.
void BM_Connection_Pipeline_Scan(benchmark::State& state) {
  StringPool pool;
  auto conn = PerfettoSqlConnection::CreateConnectionToNewDatabase(
      &pool, /*enable_extra_checks=*/false);
  auto setup = conn->Execute(SqlSource::FromExecuteQuery(R"(
    PERFETTO PRAGMA pipelines = 1;
    CREATE PERFETTO TABLE scan_values AS
    WITH RECURSIVE numbers(n) AS (
      SELECT 0 UNION ALL SELECT n + 1 FROM numbers WHERE n < 32767
    )
    SELECT n AS id, n - 32768 AS negative, n + 10000000000 AS large,
           n + 0.5 AS real_value, CASE WHEN n % 2 = 0 THEN n END AS nullable,
           'text' AS text_value FROM numbers
  )"));
  PERFETTO_CHECK(setup.ok());
  for (auto _ : state) {
    auto res = conn->Execute(SqlSource::FromExecuteQuery("FROM scan_values"));
    PERFETTO_CHECK(res.ok());
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * 32768 * 6);
}
BENCHMARK(BM_Connection_Pipeline_Scan);

// A table built from SQL: 16 integer columns c0 to c15, every other one
// nullable; `u`, unique and indexed; `ts`, sorted; and strings `s0` and `s1`,
// the second nullable. Beside it, `keys`, whose kJoinKeys rows each name a
// row of `wide`, by id in `v` and by ts in `t`.
constexpr uint32_t kWideRows = 1u << 16;
constexpr uint32_t kJoinKeys = 4096;

std::unique_ptr<PerfettoSqlConnection> MakeWide(StringPool* pool) {
  auto conn = PerfettoSqlConnection::CreateConnectionToNewDatabase(
      pool, /*enable_extra_checks=*/false);
  std::string cols;
  for (uint32_t c = 0; c < 16; ++c) {
    std::string v = "(n * 7919 + " + std::to_string(c) + ") % 1000";
    if (c % 2 == 1) {
      v = "CASE WHEN (n * 13 + " + std::to_string(c) + ") % 10 != 0 THEN " + v +
          " END";
    }
    cols += ", " + v + " AS c" + std::to_string(c);
  }
  std::string numbers =
      "WITH RECURSIVE numbers(n) AS (SELECT 0 UNION ALL "
      "SELECT n + 1 FROM numbers WHERE n < ";
  auto setup = conn->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE wide AS " + numbers +
      std::to_string(kWideRows - 1) + ") SELECT n AS id" + cols +
      ", (n * 7919) % " + std::to_string(kWideRows) +
      " AS u, n * 2 AS ts, 'name' || (n % 1000) AS s0, CASE WHEN n % 3 != 0 "
      "THEN 'thread' || (n % 97) END AS s1 FROM numbers;"
      "CREATE PERFETTO INDEX wide_u ON wide(u);"
      "CREATE PERFETTO TABLE keys AS " +
      numbers + std::to_string(kJoinKeys - 1) + ") SELECT (n * 40503) % " +
      std::to_string(kWideRows) + " AS v, (n * 40503) % " +
      std::to_string(kWideRows) + " * 2 AS t FROM numbers"));
  PERFETTO_CHECK(setup.ok());
  return conn;
}

enum class Cells { kInts, kStrings };

// Steps through every row of `sql`, which has `rows` rows, reading each
// cell. Time is per row.
void RunQuery(benchmark::State& state,
              const std::string& sql,
              uint32_t rows,
              Cells cells) {
  StringPool pool;
  auto conn = MakeWide(&pool);
  auto res = conn->ExecuteUntilLastStatement(SqlSource::FromExecuteQuery(sql));
  PERFETTO_CHECK(res.ok());
  sqlite3_stmt* stmt = res->stmt.sqlite_stmt();
  int out = sqlite3_column_count(stmt);
  int64_t sum = 0;
  uint64_t seen = 0;
  for (auto _ : state) {
    sqlite3_reset(stmt);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      ++seen;
      for (int c = 0; c < out; ++c) {
        sum += cells == Cells::kInts ? sqlite3_column_int64(stmt, c)
                                     : sqlite3_column_bytes(stmt, c);
      }
    }
  }
  PERFETTO_CHECK(seen == uint64_t{rows} * state.iterations());
  benchmark::DoNotOptimize(sum);
  state.SetItemsProcessed(static_cast<int64_t>(seen));
}

std::string WideColumns(const std::string& table, int64_t count) {
  std::string cols;
  for (int64_t c = 0; c < count; ++c) {
    cols += (c ? ", " : "") + table + "c" + std::to_string(c);
  }
  return cols;
}

// Scans: arg 0 columns read of every row of `wide`.
void BM_Connection_Wide_Scan(benchmark::State& state) {
  RunQuery(state, "SELECT " + WideColumns("", state.range(0)) + " FROM wide",
           kWideRows, Cells::kInts);
}
BENCHMARK(BM_Connection_Wide_Scan)->Arg(1)->Arg(16);

// As Wide_Scan, of the rows where a nullable column is under 500: half of
// them, scattered.
void BM_Connection_Wide_ScanFiltered(benchmark::State& state) {
  RunQuery(
      state,
      "SELECT " + WideColumns("", state.range(0)) + " FROM wide WHERE c3 < 500",
      29489, Cells::kInts);
}
BENCHMARK(BM_Connection_Wide_ScanFiltered)->Arg(1)->Arg(16);

void BM_Connection_Wide_ScanStrings(benchmark::State& state) {
  RunQuery(state, "SELECT s0, s1 FROM wide", kWideRows, Cells::kStrings);
}
BENCHMARK(BM_Connection_Wide_ScanStrings);

// Joins whose inner side is `wide`, found by the condition `on`: SQLite
// opens wide's cursor once and filters it again for each of kJoinKeys outer
// rows, reading `cols` of each match. Time is per outer row. Reading only the
// outer side, the cost to subtract, is Wide_JoinIdEq/0.
void RunJoin(benchmark::State& state,
             const std::string& cols,
             const std::string& on,
             Cells cells) {
  RunQuery(state,
           cols.empty()
               ? "SELECT k.v FROM keys k"
               : "SELECT " + cols + " FROM keys k CROSS JOIN wide w ON " + on,
           kJoinKeys, cells);
}

void BM_Connection_Wide_JoinIdEq(benchmark::State& state) {
  RunJoin(state, WideColumns("w.", state.range(0)), "w.id = k.v", Cells::kInts);
}
BENCHMARK(BM_Connection_Wide_JoinIdEq)->Arg(0)->Arg(1)->Arg(16);

// As Wide_JoinIdEq, with a condition besides the id which every row meets.
void BM_Connection_Wide_JoinIdEqFiltered(benchmark::State& state) {
  RunJoin(state, WideColumns("w.", state.range(0)), "w.id = k.v AND w.c0 >= 0",
          Cells::kInts);
}
BENCHMARK(BM_Connection_Wide_JoinIdEqFiltered)->Arg(1)->Arg(16);

void BM_Connection_Wide_JoinIdEqStrings(benchmark::State& state) {
  RunJoin(state, "w.s0, w.s1", "w.id = k.v", Cells::kStrings);
}
BENCHMARK(BM_Connection_Wide_JoinIdEqStrings);

// As Wide_JoinIdEq, found through the index on `u`.
void BM_Connection_Wide_JoinIndexEq(benchmark::State& state) {
  RunJoin(state, WideColumns("w.", state.range(0)), "w.u = k.v", Cells::kInts);
}
BENCHMARK(BM_Connection_Wide_JoinIndexEq)->Arg(1)->Arg(16);

// As Wide_JoinIdEq, found by binary search of the sorted `ts`.
void BM_Connection_Wide_JoinSortedEq(benchmark::State& state) {
  RunJoin(state, WideColumns("w.", state.range(0)), "w.ts = k.t", Cells::kInts);
}
BENCHMARK(BM_Connection_Wide_JoinSortedEq)->Arg(1)->Arg(16);

}  // namespace
}  // namespace perfetto::trace_processor
