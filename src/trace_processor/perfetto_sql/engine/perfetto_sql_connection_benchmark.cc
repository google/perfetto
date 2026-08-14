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

struct PointQueryBenchmark {
  PointQueryBenchmark() {
    connection = PerfettoSqlConnection::CreateConnectionToNewDatabase(
        &pool, /*enable_extra_checks=*/false);
    auto result = connection->Execute(SqlSource::FromExecuteQuery(R"SQL(
      CREATE PERFETTO TABLE point_query AS
      WITH RECURSIVE seq(i) AS (
        VALUES(0) UNION ALL SELECT i + 1 FROM seq WHERE i < 4095
      )
      SELECT
        i AS id,
        i % 17 AS value,
        CASE i % 2 WHEN 0 THEN 'even' ELSE 'odd' END AS name
      FROM seq
    )SQL"));
    PERFETTO_CHECK(result.ok());
  }

  SqliteConnection::PreparedStatement Prepare(const char* sql) {
    auto statement =
        connection->PrepareSqliteStatement(SqlSource::FromExecuteQuery(sql));
    PERFETTO_CHECK(statement.ok());
    return std::move(statement.value());
  }

  StringPool pool;
  std::unique_ptr<PerfettoSqlConnection> connection;
};

enum class PointQueryMode {
  kHit,
  kSecondPredicateMiss,
  kIdMiss,
};

void RunPointQueryBenchmark(benchmark::State& state,
                            PointQueryMode mode,
                            bool filter_value = true) {
  constexpr uint32_t kLookupCount = 1024;
  PointQueryBenchmark benchmark;
  const char* id_expr = mode == PointQueryMode::kIdMiss ? "5000 + i" : "i";
  const char* value_expr =
      mode == PointQueryMode::kSecondPredicateMiss ? "(i + 1) % 17" : "i % 17";
  std::string setup = R"SQL(
    CREATE TABLE lookup(id INT, value INT);
    WITH RECURSIVE seq(i) AS (
      VALUES(0) UNION ALL SELECT i + 1 FROM seq WHERE i < 1023
    )
    INSERT INTO lookup SELECT )SQL" +
                      std::string(id_expr) + ", " + value_expr + " FROM seq";
  auto setup_result =
      benchmark.connection->Execute(SqlSource::FromExecuteQuery(setup));
  PERFETTO_CHECK(setup_result.ok());

  // CROSS JOIN fixes the loop order: SQLite scans |lookup| and invokes the
  // dataframe's xFilter once for each outer row, matching real join usage.
  std::string query = R"SQL(
    SELECT sum(p.id)
    FROM lookup AS l CROSS JOIN point_query AS p
    WHERE p.id = l.id
  )SQL" + std::string(filter_value ? " AND p.value = l.value" : "");
  auto statement = benchmark.Prepare(query.c_str());
  sqlite3_stmt* stmt = statement.sqlite_stmt();

  // Run once before timing so xFilter prepares and caches the query plan.
  PERFETTO_CHECK(sqlite3_step(stmt) == SQLITE_ROW);
  benchmark::DoNotOptimize(sqlite3_column_int64(stmt, 0));
  PERFETTO_CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  PERFETTO_CHECK(sqlite3_reset(stmt) == SQLITE_OK);

  for (auto _ : state) {
    PERFETTO_CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    benchmark::DoNotOptimize(sqlite3_column_int64(stmt, 0));
    PERFETTO_CHECK(sqlite3_step(stmt) == SQLITE_DONE);
    PERFETTO_CHECK(sqlite3_reset(stmt) == SQLITE_OK);
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          kLookupCount);
}

void BM_DataframeSqliteJoin_Id_Hit(benchmark::State& state) {
  RunPointQueryBenchmark(state, PointQueryMode::kHit, false);
}
BENCHMARK(BM_DataframeSqliteJoin_Id_Hit);

void BM_DataframeSqliteJoin_Id_IdMiss(benchmark::State& state) {
  RunPointQueryBenchmark(state, PointQueryMode::kIdMiss, false);
}
BENCHMARK(BM_DataframeSqliteJoin_Id_IdMiss);

void BM_DataframeSqliteJoin_IdAndUint32_Hit(benchmark::State& state) {
  RunPointQueryBenchmark(state, PointQueryMode::kHit);
}
BENCHMARK(BM_DataframeSqliteJoin_IdAndUint32_Hit);

void BM_DataframeSqliteJoin_IdAndUint32_SecondPredicateMiss(
    benchmark::State& state) {
  RunPointQueryBenchmark(state, PointQueryMode::kSecondPredicateMiss);
}
BENCHMARK(BM_DataframeSqliteJoin_IdAndUint32_SecondPredicateMiss);

void BM_DataframeSqliteJoin_IdAndUint32_IdMiss(benchmark::State& state) {
  RunPointQueryBenchmark(state, PointQueryMode::kIdMiss);
}
BENCHMARK(BM_DataframeSqliteJoin_IdAndUint32_IdMiss);

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

}  // namespace
}  // namespace perfetto::trace_processor
