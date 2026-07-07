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

// M3a: validates that `AppendSchedDataframe` materialises a sched-schema
// dataframe into a native DuckDB table and that a real query executes *inside*
// DuckDB and returns the correct results, including the nullable string column.
//
// Data source: SYNTHETIC. Building a sched dataframe with a known, computable
// distribution lets us assert EXACT expected aggregates. Wiring the live
// `sched` dataframe out of the engine (real-trace data) is the M3b follow-up.

#include "src/trace_processor/duckdb/sched_appender.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "duckdb.h"

#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::duckdb_integration {
namespace {

// Deterministic synthetic sched row. The test both feeds these rows into the
// dataframe and recomputes the expected aggregates from the same generator, so
// the DuckDB answers can be asserted exactly.
struct SchedRow {
  int64_t ts;
  int64_t dur;
  uint32_t utid;
  std::optional<std::string> end_state;  // nullopt => SQL NULL.
  int32_t priority;
  uint32_t ucpu;
};

constexpr uint32_t kNumRows = 300;

std::vector<SchedRow> MakeRows() {
  static const char* kStates[] = {"R", "S", "D"};
  std::vector<SchedRow> rows;
  rows.reserve(kNumRows);
  for (uint32_t i = 0; i < kNumRows; ++i) {
    SchedRow r;
    r.ts = 1000 + static_cast<int64_t>(i) * 10;
    r.dur = static_cast<int64_t>(i % 7) + 1;
    r.utid = i % 5;
    // Every 4th row has a NULL end_state to exercise the validity path.
    if (i % 4 == 0) {
      r.end_state = std::nullopt;
    } else {
      r.end_state = kStates[i % 3];
    }
    r.priority = 100 - static_cast<int32_t>(i % 11);
    r.ucpu = i % 8;
    rows.push_back(std::move(r));
  }
  return rows;
}

// Builds a sched-schema dataframe from `rows` using the adhoc builder. Declares
// the 6 real sched columns (no synthetic _auto_id) so the appender sees exactly
// the sched schema.
dataframe::Dataframe BuildSchedDataframe(StringPool* pool,
                                         const std::vector<SchedRow>& rows) {
  using dataframe::AdhocColumnType;
  using dataframe::AdhocDataframeBuilder;
  AdhocDataframeBuilder::Options opts;
  opts.types = {
      AdhocColumnType::kInt64,   // ts
      AdhocColumnType::kInt64,   // dur
      AdhocColumnType::kInt64,   // utid
      AdhocColumnType::kString,  // end_state
      AdhocColumnType::kInt64,   // priority
      AdhocColumnType::kInt64,   // ucpu
  };
  // AppendSchedDataframe reads cells via random-access GetCell, which requires
  // the column to support random access. Plain kSparseNull drops its popcount
  // index at Finalize() and then FATALs on random GetCell, so use the
  // popcount-retaining variant here. (A real finalized sched column may be
  // plain SparseNull; the appender will switch to sequential Cursor reads in
  // M3b to handle that without per-row random access.)
  opts.nullability_type = dataframe::NullabilityType::kSparseNullWithPopcount;
  opts.emit_auto_id = false;
  AdhocDataframeBuilder builder(
      {"ts", "dur", "utid", "end_state", "priority", "ucpu"}, pool, opts);
  for (const auto& r : rows) {
    builder.PushNonNull(0, r.ts);
    builder.PushNonNull(1, r.dur);
    builder.PushNonNull(2, static_cast<int64_t>(r.utid));
    if (r.end_state) {
      builder.PushNonNull(3,
                          pool->InternString(base::StringView(*r.end_state)));
    } else {
      builder.PushNull(3);
    }
    builder.PushNonNull(4, static_cast<int64_t>(r.priority));
    builder.PushNonNull(5, static_cast<int64_t>(r.ucpu));
  }
  base::StatusOr<dataframe::Dataframe> df = std::move(builder).Build();
  PERFETTO_CHECK(df.ok());
  dataframe::Dataframe out = std::move(df.value());
  out.Finalize();
  return out;
}

// Runs `query` and returns the single int64 cell at (0,0). Fails the test on
// any DuckDB error.
int64_t QueryInt64(duckdb_connection con, const char* query) {
  duckdb_result res;
  EXPECT_EQ(duckdb_query(con, query, &res), DuckDBSuccess)
      << "query failed: " << query << ": " << duckdb_result_error(&res);
  int64_t v = duckdb_value_int64(&res, 0, 0);
  duckdb_destroy_result(&res);
  return v;
}

class SchedAppenderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(duckdb_open(nullptr, &db_), DuckDBSuccess);
    ASSERT_EQ(duckdb_connect(db_, &con_), DuckDBSuccess);
  }
  void TearDown() override {
    duckdb_disconnect(&con_);
    duckdb_close(&db_);
  }

  StringPool pool_;
  duckdb_database db_ = nullptr;
  duckdb_connection con_ = nullptr;
};

TEST_F(SchedAppenderTest, MaterializeAndQuery) {
  std::vector<SchedRow> rows = MakeRows();
  dataframe::Dataframe df = BuildSchedDataframe(&pool_, rows);

  base::Status status = AppendSchedDataframe(con_, df);
  ASSERT_TRUE(status.ok()) << status.message();

  // Compute expected aggregates from the same generator.
  int64_t expected_count = kNumRows;
  int64_t expected_sum_dur = 0;
  int64_t expected_non_null_end_state = 0;
  int64_t expected_count_ts_gt = 0;  // ts > 2000
  int64_t expected_sum_dur_ucpu3 = 0;
  int64_t expected_count_state_R = 0;
  for (const auto& r : rows) {
    expected_sum_dur += r.dur;
    if (r.end_state) {
      ++expected_non_null_end_state;
      if (*r.end_state == "R") {
        ++expected_count_state_R;
      }
    }
    if (r.ts > 2000) {
      ++expected_count_ts_gt;
    }
    if (r.ucpu == 3) {
      expected_sum_dur_ucpu3 += r.dur;
    }
  }

  // Full-table aggregates.
  EXPECT_EQ(QueryInt64(con_, "SELECT count(*) FROM sched"), expected_count);
  EXPECT_EQ(QueryInt64(con_, "SELECT sum(dur) FROM sched"), expected_sum_dur);

  // id is synthesised from the row offset: 0..kNumRows-1.
  EXPECT_EQ(QueryInt64(con_, "SELECT min(id) FROM sched"), 0);
  EXPECT_EQ(QueryInt64(con_, "SELECT max(id) FROM sched"), kNumRows - 1);
  EXPECT_EQ(QueryInt64(con_, "SELECT count(DISTINCT id) FROM sched"),
            expected_count);

  // String column round-trip + NULL handling.
  EXPECT_EQ(QueryInt64(con_, "SELECT count(end_state) FROM sched"),
            expected_non_null_end_state);
  EXPECT_EQ(
      QueryInt64(con_, "SELECT count(*) FROM sched WHERE end_state IS NULL"),
      expected_count - expected_non_null_end_state);
  EXPECT_EQ(
      QueryInt64(con_, "SELECT count(*) FROM sched WHERE end_state = 'R'"),
      expected_count_state_R);

  // Filtered scan executed inside DuckDB (WHERE on a numeric column).
  EXPECT_EQ(QueryInt64(con_, "SELECT count(*) FROM sched WHERE ts > 2000"),
            expected_count_ts_gt);
  EXPECT_EQ(QueryInt64(con_, "SELECT sum(dur) FROM sched WHERE ucpu = 3"),
            expected_sum_dur_ucpu3);

  // Spot-check that an individual row's columns round-tripped correctly by
  // selecting via the synthesised id.
  const SchedRow& probe = rows[42];
  EXPECT_EQ(QueryInt64(con_, "SELECT ts FROM sched WHERE id = 42"), probe.ts);
  EXPECT_EQ(QueryInt64(con_, "SELECT dur FROM sched WHERE id = 42"), probe.dur);
  EXPECT_EQ(QueryInt64(con_, "SELECT utid FROM sched WHERE id = 42"),
            static_cast<int64_t>(probe.utid));
  EXPECT_EQ(QueryInt64(con_, "SELECT priority FROM sched WHERE id = 42"),
            static_cast<int64_t>(probe.priority));
  EXPECT_EQ(QueryInt64(con_, "SELECT ucpu FROM sched WHERE id = 42"),
            static_cast<int64_t>(probe.ucpu));

  // Verify the string value for a known non-null row (row 1 => "S").
  {
    duckdb_result res;
    ASSERT_EQ(
        duckdb_query(con_, "SELECT end_state FROM sched WHERE id = 1", &res),
        DuckDBSuccess)
        << duckdb_result_error(&res);
    ASSERT_FALSE(duckdb_value_is_null(&res, 0, 0));
    char* s = duckdb_value_varchar(&res, 0, 0);
    EXPECT_STREQ(s, rows[1].end_state->c_str());
    duckdb_free(s);
    duckdb_destroy_result(&res);
  }

  // Verify row 0's end_state is NULL (generator nulls every 4th row).
  {
    duckdb_result res;
    ASSERT_EQ(
        duckdb_query(con_, "SELECT end_state FROM sched WHERE id = 0", &res),
        DuckDBSuccess)
        << duckdb_result_error(&res);
    EXPECT_TRUE(duckdb_value_is_null(&res, 0, 0));
    duckdb_destroy_result(&res);
  }
}

}  // namespace
}  // namespace perfetto::trace_processor::duckdb_integration
