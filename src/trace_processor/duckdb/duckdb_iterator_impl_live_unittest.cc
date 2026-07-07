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

// Proves that DuckDbIteratorImpl returns result rows row-for-row, column-for-
// column identical to the legacy SQLite + dataframe engine, across ints,
// strings and NULLs, and across a multi-chunk (>2048 row) boundary.
//
// Flow (mirrors sched_table_function_live_unittest.cc):
//   1. Build a TraceProcessorImpl, ingest test/data/sched_switch_original.pb.
//   2. Pull the live sched dataframe; register `sched_df` against it in DuckDB.
//   3. For each query, run it via DuckDB (duckdb_query -> wrap the result in a
//      DuckDbExecutionResult -> drive a DuckDbIteratorImpl) and via the legacy
//      engine (tp->ExecuteQuery), then assert the two row streams are equal.
//
// This validates the ITERATION half end-to-end without touching
// PerfettoSqlConnection / ExecuteQuery (that is the next subtask).

#include "src/trace_processor/duckdb/duckdb_iterator_impl.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "duckdb.h"

#include "perfetto/base/status.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/iterator.h"
#include "perfetto/trace_processor/read_trace.h"
#include "src/base/test/utils.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/duckdb/sched_table_function.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/sched_tables_py.h"
#include "src/trace_processor/trace_processor_impl.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::duckdb_integration {
namespace {

// Wraps a freshly-executed duckdb_result into a DuckDbExecutionResult
// (capturing column names + counts), exactly as the future router will. The
// returned struct owns `res`; the caller must NOT call duckdb_destroy_result on
// it (the iterator's destructor does).
DuckDbExecutionResult MakeResult(duckdb_result res, const std::string& sql) {
  DuckDbExecutionResult out;
  out.result = res;
  idx_t cols = duckdb_column_count(&out.result);
  out.column_count = static_cast<uint32_t>(cols);
  out.column_names.reserve(cols);
  for (idx_t c = 0; c < cols; ++c) {
    out.column_names.emplace_back(duckdb_column_name(&out.result, c));
  }
  out.last_statement_sql = sql;
  return out;
}

// Renders a SqlValue to a comparable string. Type-tag prefixed so a kLong 0 and
// a kNull and an empty string never compare equal by accident.
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

// Drains a legacy Iterator into a row-major vector of stringified cells.
std::vector<std::vector<std::string>> DrainLegacy(TraceProcessor* tp,
                                                  const std::string& sql,
                                                  uint32_t num_cols) {
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
  EXPECT_TRUE(it.Status().ok())
      << "legacy query failed: " << sql << ": " << it.Status().message();
  return rows;
}

// Drains a DuckDbIteratorImpl into the same shape.
std::vector<std::vector<std::string>> DrainDuck(duckdb_connection con,
                                                const std::string& sql,
                                                uint32_t num_cols) {
  std::vector<std::vector<std::string>> rows;
  duckdb_result res;
  EXPECT_EQ(duckdb_query(con, sql.c_str(), &res), DuckDBSuccess)
      << "duckdb query failed: " << sql << ": " << duckdb_result_error(&res);

  DuckDbIteratorImpl it(MakeResult(res, sql));
  while (it.Next()) {
    std::vector<std::string> row;
    row.reserve(num_cols);
    for (uint32_t c = 0; c < num_cols; ++c) {
      row.push_back(Show(it.Get(c)));
    }
    rows.push_back(std::move(row));
  }
  EXPECT_TRUE(it.Status().ok()) << "duck iterator failed: " << sql;
  return rows;
}

class DuckDbIteratorLiveTest : public ::testing::Test {
 protected:
  void SetUp() override {
    tp_ = std::make_unique<TraceProcessorImpl>(Config());
    base::Status status = ReadTrace(
        tp_.get(),
        base::GetTestDataPath("test/data/sched_switch_original.pb").c_str());
    ASSERT_TRUE(status.ok()) << status.message();

    ASSERT_EQ(duckdb_open(nullptr, &db_), DuckDBSuccess);
    ASSERT_EQ(duckdb_connect(db_, &con_), DuckDBSuccess);

    const dataframe::Dataframe& sched =
        tp_->context()->storage->mutable_sched_slice_table()->dataframe();
    ASSERT_GT(sched.row_count(), 0u) << "trace produced no sched rows";
    base::Status reg = RegisterSchedTableFunction(con_, sched);
    ASSERT_TRUE(reg.ok()) << reg.message();
  }

  void TearDown() override {
    duckdb_disconnect(&con_);
    duckdb_close(&db_);
  }

  std::unique_ptr<TraceProcessorImpl> tp_;
  duckdb_database db_ = nullptr;
  duckdb_connection con_ = nullptr;
};

// Full row-for-row, column-for-column equality across ints, a string column and
// NULLs. `end_state` has both string values and NULLs in this trace, so this
// exercises kString, kNull and kLong (id/ts/dur/utid/priority/ucpu) together.
// Both sides are ordered by id for a deterministic, comparable row order.
TEST_F(DuckDbIteratorLiveTest, MatchesLegacyAllColumns) {
  const char* cols = "id, ts, dur, utid, end_state, priority, ucpu";
  std::string legacy_sql =
      std::string("SELECT ") + cols + " FROM sched ORDER BY id";
  std::string duck_sql =
      std::string("SELECT ") + cols + " FROM sched_df() ORDER BY id";

  auto legacy = DrainLegacy(tp_.get(), legacy_sql, 7);
  auto duck = DrainDuck(con_, duck_sql, 7);

  ASSERT_GT(legacy.size(), 0u);
  ASSERT_EQ(legacy.size(), duck.size()) << "row count mismatch";
  for (size_t r = 0; r < legacy.size(); ++r) {
    ASSERT_EQ(legacy[r], duck[r]) << "row " << r << " mismatch";
  }

  // Explicit NULL assertion: this trace has NULL end_states, so the DuckDB
  // stream must contain at least one "NULL" cell (column index 4 = end_state).
  bool saw_null = false;
  bool saw_string = false;
  for (const auto& row : duck) {
    if (row[4] == "NULL") {
      saw_null = true;
    } else if (row[4].rfind("S:", 0) == 0) {
      saw_string = true;
    }
  }
  EXPECT_TRUE(saw_null) << "expected at least one NULL end_state";
  EXPECT_TRUE(saw_string) << "expected at least one non-NULL string end_state";
}

// kLong widening: count(*) surfaces as DuckDB BIGINT, and ucpu is a UINTEGER
// column. Both must be reported as kLong by the iterator (matching SQLite).
TEST_F(DuckDbIteratorLiveTest, WidensIntegersToLong) {
  // count(*) -> single BIGINT cell, reported as kLong.
  {
    duckdb_result res;
    ASSERT_EQ(duckdb_query(con_, "SELECT count(*) FROM sched_df()", &res),
              DuckDBSuccess);
    DuckDbIteratorImpl it(MakeResult(res, "count"));
    ASSERT_TRUE(it.Next());
    EXPECT_EQ(it.Get(0).type, SqlValue::kLong);
    EXPECT_FALSE(it.Next());
  }

  // ucpu is a UINTEGER column in sched_df; the iterator must widen it to kLong.
  {
    duckdb_result res;
    ASSERT_EQ(
        duckdb_query(con_, "SELECT ucpu FROM sched_df() ORDER BY id LIMIT 1",
                     &res),
        DuckDBSuccess);
    DuckDbIteratorImpl it(MakeResult(res, "ucpu"));
    ASSERT_TRUE(it.Next());
    EXPECT_EQ(it.Get(0).type, SqlValue::kLong);
  }

  // Cross-check count(*) value against the legacy engine.
  auto legacy = DrainLegacy(tp_.get(), "SELECT count(*) FROM sched", 1);
  auto duck = DrainDuck(con_, "SELECT count(*) FROM sched_df()", 1);
  ASSERT_EQ(legacy.size(), 1u);
  ASSERT_EQ(duck.size(), 1u);
  EXPECT_EQ(legacy[0][0], duck[0][0]);  // both "L:<n>"
}

// Multi-chunk boundary: cross-join sched to a range so the result has far more
// than duckdb_vector_size() (~2048) rows, forcing Next() to destroy a chunk and
// fetch the next. Asserts the total row count matches and that the string
// column stays valid (readable + non-empty for non-NULL rows) across the
// boundary.
TEST_F(DuckDbIteratorLiveTest, MultiChunkBoundary) {
  // sched has ~82 rows; cross-join with range(40) -> ~3280 rows > 2048.
  const char* duck_sql =
      "SELECT s.id, s.end_state FROM sched_df() s, range(40) "
      "ORDER BY s.id";
  const char* legacy_sql =
      "SELECT s.id, s.end_state FROM sched s, "
      "(WITH RECURSIVE r(x) AS (SELECT 0 UNION ALL SELECT x+1 FROM r "
      "WHERE x < 39) SELECT x FROM r) ORDER BY s.id";

  auto legacy = DrainLegacy(tp_.get(), legacy_sql, 2);
  auto duck = DrainDuck(con_, duck_sql, 2);

  ASSERT_GT(duck.size(), 2048u) << "expected a multi-chunk result";
  ASSERT_EQ(legacy.size(), duck.size()) << "row count mismatch across chunks";
  for (size_t r = 0; r < legacy.size(); ++r) {
    ASSERT_EQ(legacy[r], duck[r]) << "row " << r << " mismatch (multi-chunk)";
  }

  // String validity across the chunk boundary: every non-NULL end_state cell
  // anywhere in the (multi-chunk) stream must be a readable, type-correct
  // string. DrainDuck already read them while the chunk was live; re-verify the
  // tags survived.
  size_t string_cells = 0;
  for (const auto& row : duck) {
    if (row[1].rfind("S:", 0) == 0) {
      EXPECT_GT(row[1].size(), 2u) << "empty borrowed string";
      ++string_cells;
    }
  }
  EXPECT_GT(string_cells, 0u) << "no string cells across chunks";
}

// A NaN floating-point value must surface as NULL, matching SQLite (whose
// sqlite3_result_double(NaN) stores NULL - NaN is not representable). Without
// the normalization DuckDB would return a real NaN that renders as "nan",
// diverging from the SQLite goldens (e.g. args.display_value for a NaN
// real_value).
TEST_F(DuckDbIteratorLiveTest, NanDoubleSurfacesAsNull) {
  // 'nan'::DOUBLE and 'nan'::FLOAT both exercise the DOUBLE and FLOAT cases.
  auto rows =
      DrainDuck(con_,
                "SELECT CAST('nan' AS DOUBLE) AS d, CAST('nan' AS FLOAT) AS f, "
                "CAST(1.5 AS DOUBLE) AS ok",
                3);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0][0], "NULL") << "NaN DOUBLE should be NULL";
  EXPECT_EQ(rows[0][1], "NULL") << "NaN FLOAT should be NULL";
  // A normal double is unaffected.
  EXPECT_EQ(rows[0][2], "D:1.500000");
}

}  // namespace
}  // namespace perfetto::trace_processor::duckdb_integration
