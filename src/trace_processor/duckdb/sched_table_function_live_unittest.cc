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

// M3b (live wiring): proves the `sched_df` DuckDB table function scans the REAL
// sched dataframe of a loaded TraceProcessor (not synthetic data) and that
// queries executed inside DuckDB match the legacy SQLite + dataframe engine.
//
// The flow:
//   1. Build a TraceProcessorImpl and ingest a small real trace
//      (test/data/sched_switch_original.pb) that contains sched_switch events.
//   2. Pull the live sched dataframe out via
//      context()->storage->mutable_sched_slice_table()->dataframe().
//   3. Register `sched_df` over a CopyFinalized() snapshot of it in DuckDB.
//   4. Run the SAME aggregate / filter queries via the legacy engine
//      (`... FROM sched`) and via DuckDB (`... FROM sched_df()`); assert equal.
//
// Read-path note: the real finalized sched columns are NOT plain SparseNull -
// every numeric column is NonNull and `end_state` is DenseNull (see
// tables/sched_tables.py). All of these are valid for random-access
// `Dataframe::GetCell`, so the existing GetCell-based table function works
// unchanged on live data. (Plain SparseNull - which would FATAL on random
// access - does not occur for sched_slice.)

#include "src/trace_processor/duckdb/sched_table_function.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "duckdb.h"

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/iterator.h"
#include "perfetto/trace_processor/read_trace.h"
#include "src/base/test/utils.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/sched_tables_py.h"
#include "src/trace_processor/trace_processor_impl.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::duckdb_integration {
namespace {

// Runs a single-cell int64 query via the legacy TraceProcessor engine.
int64_t LegacyInt64(TraceProcessor* tp, const std::string& sql) {
  auto it = tp->ExecuteQuery(sql);
  bool has_row = it.Next();
  EXPECT_TRUE(has_row) << "no rows for: " << sql;
  SqlValue v = it.Get(0);
  int64_t result = v.is_null() ? 0 : v.AsLong();
  EXPECT_FALSE(it.Next()) << "expected single row for: " << sql;
  base::Status status = it.Status();
  EXPECT_TRUE(status.ok()) << "legacy query failed: " << sql << ": "
                           << status.message();
  return result;
}

// Runs a single-cell int64 query inside DuckDB.
int64_t DuckInt64(duckdb_connection con, const std::string& sql) {
  duckdb_result res;
  EXPECT_EQ(duckdb_query(con, sql.c_str(), &res), DuckDBSuccess)
      << "duckdb query failed: " << sql << ": " << duckdb_result_error(&res);
  int64_t v = duckdb_value_int64(&res, 0, 0);
  duckdb_destroy_result(&res);
  return v;
}

class SchedTableFunctionLiveTest : public ::testing::Test {
 protected:
  void SetUp() override {
    tp_ = std::make_unique<TraceProcessorImpl>(Config());
    base::Status status = ReadTrace(
        tp_.get(),
        base::GetTestDataPath("test/data/sched_switch_original.pb").c_str());
    ASSERT_TRUE(status.ok()) << status.message();

    ASSERT_EQ(duckdb_open(nullptr, &db_), DuckDBSuccess);
    ASSERT_EQ(duckdb_connect(db_, &con_), DuckDBSuccess);
  }

  void TearDown() override {
    duckdb_disconnect(&con_);
    duckdb_close(&db_);
  }

  const dataframe::Dataframe& SchedDataframe() {
    return tp_->context()->storage->mutable_sched_slice_table()->dataframe();
  }

  std::unique_ptr<TraceProcessorImpl> tp_;
  duckdb_database db_ = nullptr;
  duckdb_connection con_ = nullptr;
};

TEST_F(SchedTableFunctionLiveTest, MatchesLegacyEngine) {
  const dataframe::Dataframe& sched = SchedDataframe();

  // Sanity: the real trace actually produced sched rows. If this fails the
  // trace ingested no sched data and the comparison below would be vacuous.
  ASSERT_GT(sched.row_count(), 0u) << "trace produced no sched rows";

  base::Status status = RegisterSchedTableFunction(con_, sched);
  ASSERT_TRUE(status.ok()) << status.message();

  // Each pair is (the aggregate expression, run against `sched` legacy and
  // `sched_df()` in DuckDB). They must match exactly. Both engines see the same
  // underlying dataframe, so every value - including the post-scan WHERE
  // filters (the C table-function API has no filter pushdown) - must agree.
  struct Q {
    const char* legacy;  // SELECT ... FROM sched ...
    const char* duck;    // SELECT ... FROM sched_df() ...
  };
  const std::vector<Q> queries = {
      // Whole-table aggregates.
      {"SELECT count(*) FROM sched", "SELECT count(*) FROM sched_df()"},
      {"SELECT sum(dur) FROM sched", "SELECT sum(dur) FROM sched_df()"},
      {"SELECT sum(ts) FROM sched", "SELECT sum(ts) FROM sched_df()"},
      {"SELECT min(ts) FROM sched", "SELECT min(ts) FROM sched_df()"},
      {"SELECT max(ts) FROM sched", "SELECT max(ts) FROM sched_df()"},
      {"SELECT min(priority) FROM sched",
       "SELECT min(priority) FROM sched_df()"},
      {"SELECT max(priority) FROM sched",
       "SELECT max(priority) FROM sched_df()"},
      {"SELECT count(DISTINCT utid) FROM sched",
       "SELECT count(DISTINCT utid) FROM sched_df()"},
      {"SELECT count(DISTINCT ucpu) FROM sched",
       "SELECT count(DISTINCT ucpu) FROM sched_df()"},
      // String + NULL handling on end_state.
      {"SELECT count(end_state) FROM sched",
       "SELECT count(end_state) FROM sched_df()"},
      {"SELECT count(*) FROM sched WHERE end_state IS NULL",
       "SELECT count(*) FROM sched_df() WHERE end_state IS NULL"},
      {"SELECT count(*) FROM sched WHERE end_state = 'R'",
       "SELECT count(*) FROM sched_df() WHERE end_state = 'R'"},
      {"SELECT count(*) FROM sched WHERE end_state = 'S'",
       "SELECT count(*) FROM sched_df() WHERE end_state = 'S'"},
      // Numeric WHERE filters (DuckDB filters post-scan).
      {"SELECT count(*) FROM sched WHERE dur > 0",
       "SELECT count(*) FROM sched_df() WHERE dur > 0"},
      {"SELECT sum(dur) FROM sched WHERE ucpu = 0",
       "SELECT sum(dur) FROM sched_df() WHERE ucpu = 0"},
      {"SELECT count(*) FROM sched WHERE utid = 1",
       "SELECT count(*) FROM sched_df() WHERE utid = 1"},
      {"SELECT sum(dur) FROM sched WHERE priority < 120",
       "SELECT sum(dur) FROM sched_df() WHERE priority < 120"},
  };

  for (const Q& q : queries) {
    int64_t legacy = LegacyInt64(tp_.get(), q.legacy);
    int64_t duck = DuckInt64(con_, q.duck);
    EXPECT_EQ(legacy, duck)
        << "mismatch between legacy and DuckDB:\n  legacy: " << q.legacy
        << "\n  duck:   " << q.duck;
  }

  // Projection pushdown over real data: select only `dur`; the sum must still
  // match the legacy whole-column sum.
  EXPECT_EQ(
      LegacyInt64(tp_.get(), "SELECT sum(dur) FROM sched"),
      DuckInt64(con_, "SELECT sum(dur) FROM (SELECT dur FROM sched_df())"))
      << "projection-pushdown sum(dur) mismatch";

  // GROUP BY round-trip: for each ucpu, the per-cpu row count must match. Build
  // the expected map from the legacy engine, then probe DuckDB per group.
  std::vector<int64_t> ucpus;
  {
    auto it =
        tp_->ExecuteQuery("SELECT DISTINCT ucpu FROM sched ORDER BY ucpu");
    while (it.Next()) {
      ucpus.push_back(it.Get(0).AsLong());
    }
    ASSERT_TRUE(it.Status().ok()) << it.Status().message();
  }
  ASSERT_FALSE(ucpus.empty());
  for (int64_t ucpu : ucpus) {
    std::string legacy_q =
        "SELECT count(*) FROM sched WHERE ucpu = " + std::to_string(ucpu);
    std::string duck_q =
        "SELECT count(*) FROM sched_df() WHERE ucpu = " + std::to_string(ucpu);
    EXPECT_EQ(LegacyInt64(tp_.get(), legacy_q), DuckInt64(con_, duck_q))
        << "per-ucpu count mismatch for ucpu=" << ucpu;
  }
}

}  // namespace
}  // namespace perfetto::trace_processor::duckdb_integration
