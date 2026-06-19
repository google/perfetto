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
      "SELECT count(*), sum(dur), min(ts), max(ts) FROM sched";
  ExpectMatch(sql, 4);
  // The honesty counter proves DuckDB (not SQLite fallback) ran it.
  EXPECT_EQ(duck_->queries_executed_in_duckdb(), 1u);
  EXPECT_EQ(duck_->queries_fell_back_to_sqlite(), 0u);
}

TEST_F(DuckDbEngineLiveTest, FilterRunsInDuckDb) {
  ExpectMatch("SELECT count(*), sum(dur) FROM sched WHERE dur > 0", 2);
  EXPECT_EQ(duck_->queries_executed_in_duckdb(), 1u);
  EXPECT_EQ(duck_->queries_fell_back_to_sqlite(), 0u);
}

TEST_F(DuckDbEngineLiveTest, GroupByRunsInDuckDb) {
  ExpectMatch(
      "SELECT ucpu, count(*), sum(dur) FROM sched GROUP BY ucpu ORDER BY ucpu",
      3);
  EXPECT_EQ(duck_->queries_executed_in_duckdb(), 1u);
  EXPECT_EQ(duck_->queries_fell_back_to_sqlite(), 0u);
}

// A query referencing a table NOT available in DuckDB (thread) must fall back
// to SQLite with fallback ON, and still match the legacy engine.
TEST_F(DuckDbEngineLiveTest, IneligibleTableFallsBack) {
  ExpectMatch("SELECT count(*) FROM thread", 1);
  EXPECT_EQ(duck_->queries_executed_in_duckdb(), 0u);
  EXPECT_EQ(duck_->queries_fell_back_to_sqlite(), 1u);
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
    Drain(tp.get(), "SELECT count(*) FROM sched", 1, &s);
    EXPECT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(tp->queries_executed_in_duckdb(), 1u);
  }
  // Ineligible query now ERRORS (does not fall back).
  {
    base::Status s;
    Drain(tp.get(), "SELECT count(*) FROM thread", 1, &s);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(tp->queries_fell_back_to_sqlite(), 0u);
  }
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
