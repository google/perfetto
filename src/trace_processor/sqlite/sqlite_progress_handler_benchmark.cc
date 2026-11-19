/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Measures what a SQLite progress handler costs a running query when, like
// the one trace processor installs to stop interrupted queries (see
// TraceProcessorImpl::InstallInterruptCheck()), it only reads a flag. The
// query is pure SQLite work, which is the worst case for the handler's
// relative cost.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>

#include <benchmark/benchmark.h>
#include <sqlite3.h>

#include "perfetto/base/logging.h"
#include "src/trace_processor/sqlite/scoped_db.h"

namespace perfetto::trace_processor {
namespace {

bool IsBenchmarkFunctionalOnly() {
  return getenv("BENCHMARK_FUNCTIONAL_TEST_ONLY") != nullptr;
}

// The argument is the number of SQLite virtual machine steps between two
// calls of the handler: 0 runs the query without a handler.
void ProgressHandlerArgs(benchmark::internal::Benchmark* b) {
  if (IsBenchmarkFunctionalOnly()) {
    b->Arg(0)->Arg(10000);
  } else {
    b->Arg(0)->Arg(1)->Arg(100)->Arg(1000)->Arg(10000);
  }
}

int CheckInterrupted(void* ctx) {
  auto* interrupted = static_cast<std::atomic<bool>*>(ctx);
  return interrupted->load(std::memory_order_relaxed) ? 1 : 0;
}

void RunToEnd(sqlite3_stmt* stmt) {
  PERFETTO_CHECK(sqlite3_step(stmt) == SQLITE_ROW);
  benchmark::DoNotOptimize(sqlite3_column_int64(stmt, 0));
  PERFETTO_CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  PERFETTO_CHECK(sqlite3_reset(stmt) == SQLITE_OK);
}

void BM_SqliteProgressHandler(benchmark::State& state) {
  sqlite3_initialize();

  ScopedDb db;
  sqlite3* raw_db = nullptr;
  PERFETTO_CHECK(sqlite3_open(":memory:", &raw_db) == SQLITE_OK);
  db.reset(raw_db);

  // Never set, so the query always runs to the end.
  std::atomic<bool> interrupted{false};
  int steps = static_cast<int>(state.range(0));
  if (steps > 0) {
    sqlite3_progress_handler(db.get(), steps, &CheckInterrupted, &interrupted);
  }

  int64_t rows = IsBenchmarkFunctionalOnly() ? 1000 : 1000000;
  std::string sql =
      "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE "
      "x < " +
      std::to_string(rows) + ") SELECT sum(x) FROM c";
  ScopedStmt stmt;
  sqlite3_stmt* raw_stmt = nullptr;
  PERFETTO_CHECK(sqlite3_prepare_v2(db.get(), sql.c_str(), -1, &raw_stmt,
                                    nullptr) == SQLITE_OK);
  stmt.reset(raw_stmt);

  // One untimed pass, to count the steps the query takes.
  RunToEnd(stmt.get());
  int vm_steps = sqlite3_stmt_status(stmt.get(), SQLITE_STMTSTATUS_VM_STEP,
                                     /*resetFlg=*/1);

  for (auto _ : state) {
    RunToEnd(stmt.get());
  }
  state.counters["vm_steps"] =
      benchmark::Counter(static_cast<double>(vm_steps));
}
BENCHMARK(BM_SqliteProgressHandler)
    ->Apply(ProgressHandlerArgs)
    ->Unit(benchmark::kMillisecond);

}  // namespace
}  // namespace perfetto::trace_processor
