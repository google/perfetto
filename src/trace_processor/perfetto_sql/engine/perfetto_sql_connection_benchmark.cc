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

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include <benchmark/benchmark.h>

#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/perfetto_sql/engine/dataframe_module.h"
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

// Lookups by joining: each of a table's rows looks up one row of another by
// its id, and perhaps another column, as a join into a pipeline reading a
// table does, against the same join into the table in SQL. Items are
// lookups.
class Lookups {
 public:
  static constexpr uint32_t kRows = 1 << 20;
  static constexpr uint32_t kLookups = 1 << 17;

  static Lookups& Get() {
    static Lookups* lookups = new Lookups();
    return *lookups;
  }

  // Runs `sql`, its tables read by the executor or by the dataframe
  // interpreter. Items are `items` per run.
  void Run(benchmark::State& state,
           const std::string& sql,
           bool executor = true,
           uint32_t items = kLookups) {
    DataframeModule::use_executor = executor;
    for (auto _ : state) {
      auto res = conn_->Execute(SqlSource::FromExecuteQuery(sql));
      PERFETTO_CHECK(res.ok());
    }
    DataframeModule::use_executor = true;
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * items);
  }

  // The SQL a pipeline reading `big` runs as, from its FROM.
  const std::string& pipeline() const { return pipeline_; }

 private:
  Lookups()
      : conn_(PerfettoSqlConnection::CreateConnectionToNewDatabase(
            &pool_,
            /*enable_extra_checks=*/false)) {
    auto setup = conn_->Execute(SqlSource::FromExecuteQuery(R"(
      PERFETTO PRAGMA pipelines = 1;
      CREATE PERFETTO TABLE big AS
      WITH RECURSIVE n(v) AS (
        SELECT 0 UNION ALL SELECT v + 1 FROM n WHERE v < 1048575
      )
      SELECT v AS id, v * 10 AS ts, v % 8 AS cpu FROM n;
      CREATE PERFETTO TABLE outer_rows AS
      WITH RECURSIVE n(v) AS (
        SELECT 0 UNION ALL SELECT v + 1 FROM n WHERE v < 131071
      )
      SELECT (v * 7919) % 1048576 AS x FROM n;
    )"));
    PERFETTO_CHECK(setup.ok());
    auto prepared = conn_->ExecuteUntilLastStatement(
        SqlSource::FromExecuteQuery("FROM big"));
    PERFETTO_CHECK(prepared.ok());
    std::string sql = prepared->stmt.sql();
    pipeline_ = sql.substr(sql.find(" FROM ") + strlen(" FROM "));
  }

  StringPool pool_;
  std::unique_ptr<PerfettoSqlConnection> conn_;
  std::string pipeline_;
};

void BM_Connection_Lookup_Sql_Id(benchmark::State& state) {
  Lookups::Get().Run(state,
                     "SELECT COUNT(*), SUM(b.ts) FROM outer_rows o "
                     "JOIN big b ON b.id = o.x");
}
BENCHMARK(BM_Connection_Lookup_Sql_Id);

void BM_Connection_Lookup_Sql_Id_Interpreter(benchmark::State& state) {
  Lookups::Get().Run(state,
                     "SELECT COUNT(*), SUM(b.ts) FROM outer_rows o "
                     "JOIN big b ON b.id = o.x",
                     /*executor=*/false);
}
BENCHMARK(BM_Connection_Lookup_Sql_Id_Interpreter);

void BM_Connection_Lookup_Pipeline_Id(benchmark::State& state) {
  Lookups& lookups = Lookups::Get();
  lookups.Run(state, "SELECT COUNT(*), SUM(p.c1) FROM outer_rows o JOIN " +
                         lookups.pipeline() + " AS p ON p.c0 = o.x");
}
BENCHMARK(BM_Connection_Lookup_Pipeline_Id);

void BM_Connection_Lookup_Sql_IdAndCpu(benchmark::State& state) {
  Lookups::Get().Run(state,
                     "SELECT COUNT(*), SUM(b.ts) FROM outer_rows o "
                     "JOIN big b ON b.id = o.x AND b.cpu = o.x % 8");
}
BENCHMARK(BM_Connection_Lookup_Sql_IdAndCpu);

void BM_Connection_Lookup_Sql_IdAndCpu_Interpreter(benchmark::State& state) {
  Lookups::Get().Run(state,
                     "SELECT COUNT(*), SUM(b.ts) FROM outer_rows o "
                     "JOIN big b ON b.id = o.x AND b.cpu = o.x % 8",
                     /*executor=*/false);
}
BENCHMARK(BM_Connection_Lookup_Sql_IdAndCpu_Interpreter);

void BM_Connection_Lookup_Pipeline_IdAndCpu(benchmark::State& state) {
  Lookups& lookups = Lookups::Get();
  lookups.Run(state, "SELECT COUNT(*), SUM(p.c1) FROM outer_rows o JOIN " +
                         lookups.pipeline() +
                         " AS p ON p.c0 = o.x AND p.c2 = o.x % 8");
}
BENCHMARK(BM_Connection_Lookup_Pipeline_IdAndCpu);

// Scans of a table in SQL, filtered, read by the executor and by the
// dataframe interpreter. Items are the table's rows.
constexpr char kScanUnsorted[] =
    "SELECT COUNT(*), SUM(ts) FROM big WHERE cpu = 3";
constexpr char kScanSorted[] =
    "SELECT COUNT(*), SUM(cpu) FROM big WHERE ts >= 1000000 AND ts < 2000000";

void BM_Connection_Scan_Sql_Unsorted(benchmark::State& state) {
  Lookups::Get().Run(state, kScanUnsorted, /*executor=*/true, Lookups::kRows);
}
BENCHMARK(BM_Connection_Scan_Sql_Unsorted);

void BM_Connection_Scan_Sql_Unsorted_Interpreter(benchmark::State& state) {
  Lookups::Get().Run(state, kScanUnsorted, /*executor=*/false, Lookups::kRows);
}
BENCHMARK(BM_Connection_Scan_Sql_Unsorted_Interpreter);

void BM_Connection_Scan_Sql_Sorted(benchmark::State& state) {
  Lookups::Get().Run(state, kScanSorted, /*executor=*/true, Lookups::kRows);
}
BENCHMARK(BM_Connection_Scan_Sql_Sorted);

void BM_Connection_Scan_Sql_Sorted_Interpreter(benchmark::State& state) {
  Lookups::Get().Run(state, kScanSorted, /*executor=*/false, Lookups::kRows);
}
BENCHMARK(BM_Connection_Scan_Sql_Sorted_Interpreter);

}  // namespace
}  // namespace perfetto::trace_processor
