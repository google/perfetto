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

// D2 subtask 2 (the routing fork): proves a REAL `ExecuteQuery("... FROM
// sched")` runs inside DuckDB end-to-end, matches the legacy SQLite engine, and
// is measured honestly (the per-query DuckDB-vs-fallback counters + the
// fallback-disabled error gate).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/iterator.h"
#include "perfetto/trace_processor/read_trace.h"
#include "src/base/test/utils.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/sched_tables_py.h"
#include "src/trace_processor/trace_processor_impl.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

std::string Show(const SqlValue& v) {
  switch (v.type) {
    case SqlValue::kNull:
      return "NULL";
    case SqlValue::kLong:
      return "L:" + std::to_string(v.AsLong());
    case SqlValue::kDouble:
      return "D:" + std::to_string(v.AsDouble());
    case SqlValue::kString:
      return std::string("S:") + v.AsString();
    case SqlValue::kBytes:
      return "B:" + std::to_string(v.bytes_count);
  }
  return "?";
}

std::vector<std::vector<std::string>> Drain(TraceProcessor* tp,
                                            const std::string& sql,
                                            uint32_t num_cols,
                                            base::Status* status_out) {
  std::vector<std::vector<std::string>> rows;
  auto it = tp->ExecuteQuery(sql);
  while (it.Next()) {
    std::vector<std::string> row;
    row.reserve(num_cols);
    for (uint32_t c = 0; c < num_cols; ++c) {
      row.push_back(Show(it.Get(c)));
    }
    rows.push_back(std::move(row));
  }
  *status_out = it.Status();
  return rows;
}

class DuckDbEngineLiveTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Config duck_cfg;
    duck_cfg.enable_duckdb_query_engine = true;
    duck_ = std::make_unique<TraceProcessorImpl>(duck_cfg);
    legacy_ = std::make_unique<TraceProcessorImpl>(Config());

    for (TraceProcessorImpl* tp : {duck_.get(), legacy_.get()}) {
      base::Status status = ReadTrace(
          tp, base::GetTestDataPath("test/data/sched_switch_original.pb")
                  .c_str());
      ASSERT_TRUE(status.ok()) << status.message();
    }
    ASSERT_GT(duck_->context()->storage->sched_slice_table().row_count(), 0u);
  }

  // Runs `sql` on both engines and asserts equal result sets.
  void ExpectMatch(const std::string& sql, uint32_t num_cols) {
    base::Status s_duck;
    base::Status s_legacy;
    auto duck = Drain(duck_.get(), sql, num_cols, &s_duck);
    auto legacy = Drain(legacy_.get(), sql, num_cols, &s_legacy);
    ASSERT_TRUE(s_duck.ok()) << "duck: " << sql << ": " << s_duck.message();
    ASSERT_TRUE(s_legacy.ok()) << "legacy: " << sql << ": "
                               << s_legacy.message();
    ASSERT_EQ(legacy.size(), duck.size()) << "row count mismatch: " << sql;
    for (size_t r = 0; r < legacy.size(); ++r) {
      ASSERT_EQ(legacy[r], duck[r]) << "row " << r << " mismatch: " << sql;
    }
  }

  std::unique_ptr<TraceProcessorImpl> duck_;
  std::unique_ptr<TraceProcessorImpl> legacy_;
};

TEST_F(DuckDbEngineLiveTest, AggregatesRunInDuckDb) {
  const std::string sql =
      "SELECT count(*) AS c, sum(dur) AS s, min(ts) AS mn, max(ts) AS mx "
      "FROM sched";
  ExpectMatch(sql, 4);
  // The honesty counter proves DuckDB (not SQLite fallback) ran it.
  EXPECT_EQ(duck_->queries_executed_in_duckdb(), 1u);
  EXPECT_EQ(duck_->queries_fell_back_to_sqlite(), 0u);
}

TEST_F(DuckDbEngineLiveTest, FilterRunsInDuckDb) {
  ExpectMatch("SELECT count(*) AS c, sum(dur) AS s FROM sched WHERE dur > 0",
              2);
  EXPECT_EQ(duck_->queries_executed_in_duckdb(), 1u);
  EXPECT_EQ(duck_->queries_fell_back_to_sqlite(), 0u);
}

TEST_F(DuckDbEngineLiveTest, GroupByRunsInDuckDb) {
  ExpectMatch(
      "SELECT ucpu, count(*) AS c, sum(dur) AS s FROM sched GROUP BY ucpu "
      "ORDER BY ucpu",
      3);
  EXPECT_EQ(duck_->queries_executed_in_duckdb(), 1u);
  EXPECT_EQ(duck_->queries_fell_back_to_sqlite(), 0u);
}

// A query referencing a relation DuckDB cannot bind (a genuinely-nonexistent
// table - NOT sqlite_master, which DuckDB ALSO provides as a compat view with
// divergent content) routes to DuckDB, gets a Catalog/Binder error, and falls
// back to SQLite with fallback ON, matching the legacy engine (both error: no
// such table). The relation gate is now delegated to DuckDB's binder.
TEST_F(DuckDbEngineLiveTest, IneligibleTableFallsBack) {
  base::Status s_duck;
  base::Status s_legacy;
  Drain(duck_.get(), "SELECT count(*) FROM not_a_real_table_xyz", 1, &s_duck);
  Drain(legacy_.get(), "SELECT count(*) FROM not_a_real_table_xyz", 1,
        &s_legacy);
  // Both engines error (no such table). Fallback handled the DuckDB-side error.
  EXPECT_FALSE(s_duck.ok());
  EXPECT_FALSE(s_legacy.ok());
  EXPECT_EQ(duck_->queries_executed_in_duckdb(), 0u);
  EXPECT_EQ(duck_->queries_fell_back_to_sqlite(), 1u);
}

// Wave 2: the `thread` PerfettoSQL view is mirrored into DuckDB, so a bare
// `FROM thread` resolves through DuckDB's catalog (view body -> __intrinsic_*
// -> replacement scan) and runs ENTIRELY in DuckDB, matching legacy.
TEST_F(DuckDbEngineLiveTest, MirroredViewRunsInDuckDb) {
  ExpectMatch("SELECT count(*) AS c FROM thread", 1);
  EXPECT_EQ(duck_->queries_executed_in_duckdb(), 1u);
  EXPECT_EQ(duck_->queries_fell_back_to_sqlite(), 0u);
}

// Wave 2: a multi-statement query (INCLUDE ...; SELECT ... FROM <runtime
// table>) runs the leading INCLUDE in the engine, then the final SELECT in
// DuckDB. This is the StdlibSched off-zero shape.
TEST_F(DuckDbEngineLiveTest, IncludeThenSelectRunsFinalInDuckDb) {
  ExpectMatch(
      "INCLUDE PERFETTO MODULE sched.thread_level_parallelism;\n"
      "SELECT count(*) AS c FROM sched_runnable_thread_count",
      1);
  EXPECT_EQ(duck_->queries_executed_in_duckdb(), 1u);
  EXPECT_EQ(duck_->queries_fell_back_to_sqlite(), 0u);
}

// A query using a function not in the allowlist (a custom intrinsic) falls back.
TEST_F(DuckDbEngineLiveTest, IneligibleFunctionFallsBack) {
  // group_concat is a real SQLite aggregate but not in the DuckDB allowlist.
  ExpectMatch("SELECT group_concat(end_state) FROM sched", 1);
  EXPECT_EQ(duck_->queries_executed_in_duckdb(), 0u);
  EXPECT_EQ(duck_->queries_fell_back_to_sqlite(), 1u);
}

// The honesty gate: with fallback DISABLED, an ineligible query ERRORS instead
// of silently using SQLite, so a measurement lane can trust the pass count.
TEST_F(DuckDbEngineLiveTest, FallbackDisabledErrorsOnIneligible) {
  Config cfg;
  cfg.enable_duckdb_query_engine = true;
  cfg.duckdb_disable_fallback = true;
  auto tp = std::make_unique<TraceProcessorImpl>(cfg);
  base::Status status = ReadTrace(
      tp.get(),
      base::GetTestDataPath("test/data/sched_switch_original.pb").c_str());
  ASSERT_TRUE(status.ok()) << status.message();

  // Eligible query still runs in DuckDB.
  {
    base::Status s;
    Drain(tp.get(), "SELECT count(*) AS c FROM sched", 1, &s);
    EXPECT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(tp->queries_executed_in_duckdb(), 1u);
  }
  // A query DuckDB cannot run (a nonexistent relation -> Catalog/Binder error)
  // now ERRORS instead of silently falling back, so a measurement lane can
  // trust the pass count.
  {
    base::Status s;
    Drain(tp.get(), "SELECT count(*) FROM not_a_real_table_xyz", 1, &s);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(tp->queries_fell_back_to_sqlite(), 0u);
  }
}

// Regression test for the DuckDB string-lifetime bug (previously misdiagnosed
// as a "StringPool corruption" that forced the IsSafeToMirror allowlist). Two
// distinct defects are covered, both surfacing as garbage in the shell's
// PrintStats output (`...<garbage>: <value>`):
//   1. A `duckdb_string_t` is NOT NUL-terminated; returning its raw data
//      pointer as a C string made consumers doing `%s`/strlen read past the end.
//   2. The per-column owned-copy fix must NOT reallocate its buffer vector
//      mid-row: reading a LATER column must not dangle an EARLIER column's
//      `const char*` (short SSO strings live inside the moved std::string).
// This test mimics PrintStats EXACTLY: `SELECT name, idx, value, description
// FROM stats`, reading the string columns `name` (col 0) and `description`
// (col 3) of the same row, and compares against the legacy engine. It fails on
// either defect and proves the `stats` view is now safe to mirror.
TEST_F(DuckDbEngineLiveTest, StatsViewStringsAreStableAndNulTerminated) {
  const std::string sql =
      "SELECT name, idx, value, description FROM stats "
      "WHERE severity = 'info' ORDER BY name, idx";

  auto collect = [](TraceProcessor* tp, const std::string& q,
                    base::Status* status) {
    std::vector<std::pair<std::string, std::string>> rows;
    auto it = tp->ExecuteQuery(q);
    while (it.Next()) {
      const char* name = it.Get(0).string_value;          // col 0 (string)
      const char* desc = it.Get(3).string_value;          // col 3 (string)
      // Read name AFTER desc to exercise the cross-column stability: both must
      // remain valid C strings simultaneously.
      std::string n = name ? std::string(name) : std::string("<null>");
      std::string d = desc ? std::string(desc) : std::string("<null>");
      rows.emplace_back(std::move(n), std::move(d));
    }
    *status = it.Status();
    return rows;
  };

  base::Status s_duck;
  base::Status s_legacy;
  auto duck_rows = collect(duck_.get(), sql, &s_duck);
  auto legacy_rows = collect(legacy_.get(), sql, &s_legacy);
  ASSERT_TRUE(s_duck.ok()) << s_duck.message();
  ASSERT_TRUE(s_legacy.ok()) << s_legacy.message();
  EXPECT_GT(duck_->queries_executed_in_duckdb(), 0u);
  ASSERT_FALSE(legacy_rows.empty());
  EXPECT_EQ(duck_rows, legacy_rows);
}

// With the flag OFF, ExecuteQuery is the legacy path: a couple of sanity queries
// match and nothing is routed to DuckDB.
TEST_F(DuckDbEngineLiveTest, FlagOffIsLegacy) {
  base::Status s1;
  base::Status s2;
  auto a = Drain(legacy_.get(), "SELECT count(*) FROM sched", 1, &s1);
  auto b = Drain(legacy_.get(), "SELECT count(*) FROM thread", 1, &s2);
  EXPECT_TRUE(s1.ok());
  EXPECT_TRUE(s2.ok());
  EXPECT_EQ(legacy_->queries_executed_in_duckdb(), 0u);
  EXPECT_EQ(legacy_->queries_fell_back_to_sqlite(), 0u);
}

}  // namespace
}  // namespace perfetto::trace_processor
