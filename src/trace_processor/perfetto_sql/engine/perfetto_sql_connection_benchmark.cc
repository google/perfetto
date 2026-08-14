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

class PointJoinBenchmark {
 public:
  PointJoinBenchmark()
      : connection_(PerfettoSqlConnection::CreateConnectionToNewDatabase(
            &pool_,
            /*enable_extra_checks=*/false)) {
    auto status = connection_->Execute(SqlSource::FromExecuteQuery(R"SQL(
      CREATE PERFETTO TABLE point_table AS
      WITH RECURSIVE sequence(id) AS (
        VALUES(0)
        UNION ALL
        SELECT id + 1 FROM sequence WHERE id < 4095
      )
      SELECT id, id % 17 AS value FROM sequence
    )SQL"));
    PERFETTO_CHECK(status.ok());
  }

  PerfettoSqlConnection* connection() { return connection_.get(); }

 private:
  StringPool pool_;
  std::unique_ptr<PerfettoSqlConnection> connection_;
};

enum class PointJoinResult {
  kHit,
  kValueMiss,
  kIdMiss,
};

void RunPointJoinBenchmark(benchmark::State& state,
                           PointJoinResult result,
                           bool check_value = true) {
  constexpr int64_t kLookupCount = 1024;
  PointJoinBenchmark benchmark;

  const char* id = result == PointJoinResult::kIdMiss ? "id + 5000" : "id";
  const char* value =
      result == PointJoinResult::kValueMiss ? "(id + 1) % 17" : "id % 17";
  std::string setup = R"SQL(
    CREATE TABLE lookup(id INTEGER, value INTEGER);
    WITH RECURSIVE sequence(id) AS (
      VALUES(0)
      UNION ALL
      SELECT id + 1 FROM sequence WHERE id < 1023
    )
    INSERT INTO lookup SELECT )SQL" +
                      std::string(id) + ", " + value + " FROM sequence";
  auto status =
      benchmark.connection()->Execute(SqlSource::FromExecuteQuery(setup));
  PERFETTO_CHECK(status.ok());

  std::string query = R"SQL(
    SELECT sum(point_table.id)
    FROM lookup CROSS JOIN point_table
    WHERE point_table.id = lookup.id
  )SQL";
  if (check_value) {
    query += " AND point_table.value = lookup.value";
  }
  auto prepared = benchmark.connection()->PrepareSqliteStatement(
      SqlSource::FromExecuteQuery(query));
  PERFETTO_CHECK(prepared.ok());
  sqlite3_stmt* stmt = prepared->sqlite_stmt();

  // Exclude one-time virtual table planning from repeated execution.
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

void BM_SqliteJoin_Id_Hit(benchmark::State& state) {
  RunPointJoinBenchmark(state, PointJoinResult::kHit, false);
}
BENCHMARK(BM_SqliteJoin_Id_Hit);

void BM_SqliteJoin_Id_IdMiss(benchmark::State& state) {
  RunPointJoinBenchmark(state, PointJoinResult::kIdMiss, false);
}
BENCHMARK(BM_SqliteJoin_Id_IdMiss);

void BM_SqliteJoin_IdAndValue_Hit(benchmark::State& state) {
  RunPointJoinBenchmark(state, PointJoinResult::kHit);
}
BENCHMARK(BM_SqliteJoin_IdAndValue_Hit);

void BM_SqliteJoin_IdAndValue_ValueMiss(benchmark::State& state) {
  RunPointJoinBenchmark(state, PointJoinResult::kValueMiss);
}
BENCHMARK(BM_SqliteJoin_IdAndValue_ValueMiss);

void BM_SqliteJoin_IdAndValue_IdMiss(benchmark::State& state) {
  RunPointJoinBenchmark(state, PointJoinResult::kIdMiss);
}
BENCHMARK(BM_SqliteJoin_IdAndValue_IdMiss);

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
