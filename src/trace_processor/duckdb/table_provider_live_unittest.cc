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

// D1 (first subtask): validates the generic, data-driven
// `__perfetto_df(VARCHAR)` DuckDB table function. It scans a
// `dataframe::Dataframe` by name through the dataframe `Cursor` (NOT
// `Dataframe::GetCell`), so it handles every nullability kind - including plain
// `SparseNull`, which `GetCell` FATALs on (dataframe.h:357-359).
//
// Two cases:
//   1. MatchesLegacyEngine: real sched data, row-for-row vs the legacy engine.
//   2. PlainSparseNull: a synthetic dataframe with a plain `kSparseNull`
//   nullable
//      column carrying KNOWN values with nulls. This is THE load-bearing case:
//      the previous `sched_df` path used `GetCell`, which FATALs on plain
//      SparseNull; the cursor path must read it correctly. The synthetic data
//      spans > duckdb_vector_size() rows to exercise chunk boundaries.

#include "src/trace_processor/duckdb/table_provider.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "duckdb.h"

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/iterator.h"
#include "perfetto/trace_processor/read_trace.h"
#include "src/base/test/utils.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/sched_tables_py.h"
#include "src/trace_processor/trace_processor_impl.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::duckdb_integration {
namespace {

// ----------------------------------------------------------------------------
// Case 1: real sched data, row-for-row vs the legacy engine.
// ----------------------------------------------------------------------------

class DuckDbTableProviderLiveTest : public ::testing::Test {
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

// Builds a small synthetic two-column table ("id" Id column + "v" Int64) so the
// Live fixture has a SECOND registered table alongside `sched`.
dataframe::Dataframe BuildSecondTable(StringPool* pool) {
  using dataframe::AdhocColumnType;
  using dataframe::AdhocDataframeBuilder;
  AdhocDataframeBuilder::Options opts;
  opts.types = {AdhocColumnType::kInt64};
  opts.nullability_type = dataframe::NullabilityType::kSparseNull;
  opts.emit_auto_id = true;
  AdhocDataframeBuilder builder({"v"}, pool, opts);
  for (int64_t i = 0; i < 10; ++i) {
    builder.PushNonNull(0, i * 100);
  }
  base::StatusOr<dataframe::Dataframe> df = std::move(builder).Build();
  PERFETTO_CHECK(df.ok());
  dataframe::Dataframe out = std::move(df.value());
  out.Finalize();
  return out;
}

TEST_F(DuckDbTableProviderLiveTest, MatchesLegacyEngine) {
  const dataframe::Dataframe& sched = SchedDataframe();
  ASSERT_GT(sched.row_count(), 0u) << "trace produced no sched rows";

  // The provider needs the StringPool that backs the dataframe so it can
  // resolve String cells (end_state). It is the TraceProcessor's storage
  // StringPool.
  StringPool* pool = tp_->context()->storage->mutable_string_pool();
  dataframe::Dataframe second = BuildSecondTable(pool);
  DuckDbTableProvider provider(pool);
  ASSERT_TRUE(provider.Register("sched", sched).ok());
  ASSERT_TRUE(provider.Register("second_tbl", second).ok());
  ASSERT_TRUE(provider.RegisterTableFunction(con_).ok());
  ASSERT_TRUE(provider.RegisterReplacementScan(db_).ok());

  // Pull the same projection from both engines, ordered by id, and compare cell
  // for cell. id is an `Id` storage column (== row index), so storage order ==
  // id order; we order both sides by id to be deterministic regardless.
  const char* kCols = "id, ts, dur, utid, end_state, priority, ucpu";

  // Legacy result: read into a row-major vector of (int64 + null) / string.
  struct Cell {
    bool is_null = false;
    int64_t i = 0;
    std::string s;
    bool is_string = false;
  };
  std::vector<std::vector<Cell>> legacy_rows;
  {
    auto it = tp_->ExecuteQuery(std::string("SELECT ") + kCols +
                                " FROM sched ORDER BY id");
    while (it.Next()) {
      std::vector<Cell> row;
      for (uint32_t c = 0; c < 7; ++c) {
        SqlValue v = it.Get(c);
        Cell cell;
        if (v.is_null()) {
          cell.is_null = true;
        } else if (v.type == SqlValue::kString) {
          cell.is_string = true;
          cell.s = v.AsString();
        } else {
          cell.i = v.AsLong();
        }
        row.push_back(std::move(cell));
      }
      legacy_rows.push_back(std::move(row));
    }
    ASSERT_TRUE(it.Status().ok()) << it.Status().message();
  }
  ASSERT_EQ(legacy_rows.size(), sched.row_count());

  duckdb_result res;
  ASSERT_EQ(duckdb_query(con_,
                         (std::string("SELECT ") + kCols +
                          " FROM __perfetto_df('sched') ORDER BY id")
                             .c_str(),
                         &res),
            DuckDBSuccess)
      << duckdb_result_error(&res);
  ASSERT_EQ(duckdb_row_count(&res), legacy_rows.size());
  ASSERT_EQ(duckdb_column_count(&res), 7u);

  for (idx_t r = 0; r < legacy_rows.size(); ++r) {
    const std::vector<Cell>& lrow = legacy_rows[r];
    for (idx_t c = 0; c < 7; ++c) {
      bool duck_null = duckdb_value_is_null(&res, c, r);
      EXPECT_EQ(duck_null, lrow[c].is_null)
          << "null mismatch at row " << r << " col " << c;
      if (lrow[c].is_null) {
        continue;
      }
      if (lrow[c].is_string) {
        char* s = duckdb_value_varchar(&res, c, r);
        EXPECT_STREQ(s, lrow[c].s.c_str())
            << "string mismatch at row " << r << " col " << c;
        duckdb_free(s);
      } else {
        EXPECT_EQ(duckdb_value_int64(&res, c, r), lrow[c].i)
            << "int mismatch at row " << r << " col " << c;
      }
    }
  }
  duckdb_destroy_result(&res);

  // BARE `FROM sched` via the replacement scan must return rows identical to
  // both `__perfetto_df('sched')` and the legacy engine.
  duckdb_result bare;
  ASSERT_EQ(
      duckdb_query(
          con_,
          (std::string("SELECT ") + kCols + " FROM sched ORDER BY id").c_str(),
          &bare),
      DuckDBSuccess)
      << duckdb_result_error(&bare);
  ASSERT_EQ(duckdb_row_count(&bare), legacy_rows.size());
  ASSERT_EQ(duckdb_column_count(&bare), 7u);
  for (idx_t r = 0; r < legacy_rows.size(); ++r) {
    const std::vector<Cell>& lrow = legacy_rows[r];
    for (idx_t c = 0; c < 7; ++c) {
      EXPECT_EQ(duckdb_value_is_null(&bare, c, r), lrow[c].is_null)
          << "bare null mismatch at row " << r << " col " << c;
      if (lrow[c].is_null) {
        continue;
      }
      if (lrow[c].is_string) {
        char* s = duckdb_value_varchar(&bare, c, r);
        EXPECT_STREQ(s, lrow[c].s.c_str())
            << "bare string mismatch at row " << r << " col " << c;
        duckdb_free(s);
      } else {
        EXPECT_EQ(duckdb_value_int64(&bare, c, r), lrow[c].i)
            << "bare int mismatch at row " << r << " col " << c;
      }
    }
  }
  duckdb_destroy_result(&bare);

  // The SECOND registered table also resolves bare, proving the replacement
  // scan is generic (not sched-specific).
  duckdb_result second_res;
  ASSERT_EQ(duckdb_query(con_, "SELECT count(*), sum(v) FROM second_tbl",
                         &second_res),
            DuckDBSuccess)
      << duckdb_result_error(&second_res);
  EXPECT_EQ(duckdb_value_int64(&second_res, 0, 0), 10);
  EXPECT_EQ(duckdb_value_int64(&second_res, 1, 0), 4500);  // sum 0,100..900
  duckdb_destroy_result(&second_res);

  // MISS path: a bare reference to an unknown name must produce a clean DuckDB
  // error (DuckDBError), NOT a crash. The replacement scan leaves the function
  // name unset so DuckDB raises its normal catalog error.
  duckdb_result miss;
  EXPECT_EQ(duckdb_query(con_, "SELECT * FROM definitely_not_a_table", &miss),
            DuckDBError);
  duckdb_destroy_result(&miss);
}

// ----------------------------------------------------------------------------
// Read-through resolver: a name NOT pre-registered is supplied lazily by the
// resolver on first reference (snapshot-on-miss).
// ----------------------------------------------------------------------------

TEST(DuckDbTableProviderResolverTest, LazySnapshotOnMiss) {
  StringPool pool;
  dataframe::Dataframe lazy = BuildSecondTable(&pool);

  duckdb_database db = nullptr;
  duckdb_connection con = nullptr;
  ASSERT_EQ(duckdb_open(nullptr, &db), DuckDBSuccess);
  ASSERT_EQ(duckdb_connect(db, &con), DuckDBSuccess);

  // The provider starts with NOTHING registered; the resolver supplies "lazy"
  // (and only "lazy") on demand. This mirrors the engine's GetDataframeOrNull
  // read-through that a later subtask will wire into PerfettoSqlConnection.
  int resolver_calls = 0;
  DuckDbTableProvider provider(
      &pool, [&](const std::string& name) -> const dataframe::Dataframe* {
        ++resolver_calls;
        return name == "lazy" ? &lazy : nullptr;
      });
  ASSERT_TRUE(provider.RegisterTableFunction(con).ok());
  ASSERT_TRUE(provider.RegisterReplacementScan(db).ok());

  // Pre-condition: not in the local cache yet.
  ASSERT_EQ(provider.Find("lazy"), nullptr);

  // Bare `FROM lazy` resolves through the resolver, snapshots, and scans.
  duckdb_result res;
  ASSERT_EQ(duckdb_query(con, "SELECT count(*), sum(v) FROM lazy", &res),
            DuckDBSuccess)
      << duckdb_result_error(&res);
  EXPECT_EQ(duckdb_value_int64(&res, 0, 0), 10);
  EXPECT_EQ(duckdb_value_int64(&res, 1, 0), 4500);
  duckdb_destroy_result(&res);

  // Post-condition: the snapshot is now cached (lazy snapshot-on-miss worked).
  EXPECT_NE(provider.Find("lazy"), nullptr);
  EXPECT_GT(resolver_calls, 0);

  // A name the resolver does NOT know still errors cleanly.
  duckdb_result miss;
  EXPECT_EQ(duckdb_query(con, "SELECT * FROM unknown_name", &miss),
            DuckDBError);
  duckdb_destroy_result(&miss);

  duckdb_disconnect(&con);
  duckdb_close(&db);
}

// ----------------------------------------------------------------------------
// Case 2: THE load-bearing case - a plain `kSparseNull` column.
// ----------------------------------------------------------------------------

struct SparseRow {
  int64_t key;                      // NonNull int column.
  std::optional<int64_t> opt_val;   // plain SparseNull int column.
  std::optional<std::string> name;  // plain SparseNull string column.
};

// > duckdb_vector_size() (~2048) so the scan spans multiple chunks.
constexpr uint32_t kSparseRows = 5000;

std::vector<SparseRow> MakeSparseRows() {
  std::vector<SparseRow> rows;
  rows.reserve(kSparseRows);
  for (uint32_t i = 0; i < kSparseRows; ++i) {
    SparseRow r;
    r.key = static_cast<int64_t>(i);
    // Null out roughly every 3rd int value and every 5th string.
    r.opt_val = (i % 3 == 0)
                    ? std::nullopt
                    : std::make_optional<int64_t>(static_cast<int64_t>(i) * 7);
    r.name = (i % 5 == 0)
                 ? std::nullopt
                 : std::make_optional<std::string>("n" + std::to_string(i % 4));
    rows.push_back(std::move(r));
  }
  return rows;
}

dataframe::Dataframe BuildSparseDataframe(StringPool* pool,
                                          const std::vector<SparseRow>& rows) {
  using dataframe::AdhocColumnType;
  using dataframe::AdhocDataframeBuilder;
  AdhocDataframeBuilder::Options opts;
  opts.types = {
      AdhocColumnType::kInt64,   // key (NonNull, all values present)
      AdhocColumnType::kInt64,   // opt_val (nullable)
      AdhocColumnType::kString,  // name (nullable)
  };
  // PLAIN SparseNull (NOT the popcount-retaining variant). This is the case
  // `Dataframe::GetCell` FATALs on after Finalize() - the whole point of this
  // test. The cursor reads it correctly because it walks rows in storage order
  // via the query plan's bytecode rather than doing random access.
  opts.nullability_type = dataframe::NullabilityType::kSparseNull;
  // emit_auto_id => the builder appends an `_auto_id` Id column the provider
  // resolves as the table's id column.
  opts.emit_auto_id = true;
  AdhocDataframeBuilder builder({"key", "opt_val", "name"}, pool, opts);
  for (const auto& r : rows) {
    builder.PushNonNull(0, r.key);
    if (r.opt_val) {
      builder.PushNonNull(1, *r.opt_val);
    } else {
      builder.PushNull(1);
    }
    if (r.name) {
      builder.PushNonNull(2, pool->InternString(base::StringView(*r.name)));
    } else {
      builder.PushNull(2);
    }
  }
  base::StatusOr<dataframe::Dataframe> df = std::move(builder).Build();
  PERFETTO_CHECK(df.ok());
  dataframe::Dataframe out = std::move(df.value());
  out.Finalize();
  return out;
}

TEST(DuckDbTableProviderSparseTest, PlainSparseNull) {
  StringPool pool;
  std::vector<SparseRow> rows = MakeSparseRows();
  dataframe::Dataframe df = BuildSparseDataframe(&pool, rows);

  // Sanity: confirm the nullable columns really are plain SparseNull after
  // finalize (GetNullabilityLegacy index for plain SparseNull). If this ever
  // regresses to a popcount variant the test would no longer cover the FATAL
  // case it is meant to guard.
  ASSERT_EQ(df.GetNullabilityLegacy(1).index(),
            dataframe::Nullability::GetTypeIndex<dataframe::SparseNull>());
  ASSERT_EQ(df.GetNullabilityLegacy(2).index(),
            dataframe::Nullability::GetTypeIndex<dataframe::SparseNull>());

  duckdb_database db = nullptr;
  duckdb_connection con = nullptr;
  ASSERT_EQ(duckdb_open(nullptr, &db), DuckDBSuccess);
  ASSERT_EQ(duckdb_connect(db, &con), DuckDBSuccess);

  DuckDbTableProvider provider(&pool);
  ASSERT_TRUE(provider.Register("t", df).ok());
  ASSERT_TRUE(provider.RegisterTableFunction(con).ok());

  // Scan ALL columns; the scan must NOT crash/FATAL and must return exactly the
  // known values + nulls. Order by key (== row order) for determinism.
  duckdb_result res;
  ASSERT_EQ(
      duckdb_query(
          con, "SELECT key, opt_val, name FROM __perfetto_df('t') ORDER BY key",
          &res),
      DuckDBSuccess)
      << duckdb_result_error(&res);
  ASSERT_EQ(duckdb_row_count(&res), kSparseRows);

  for (uint32_t i = 0; i < kSparseRows; ++i) {
    const SparseRow& r = rows[i];
    EXPECT_FALSE(duckdb_value_is_null(&res, 0, i)) << "key null at " << i;
    EXPECT_EQ(duckdb_value_int64(&res, 0, i), r.key) << "key mismatch at " << i;

    if (r.opt_val) {
      EXPECT_FALSE(duckdb_value_is_null(&res, 1, i)) << "opt_val null at " << i;
      EXPECT_EQ(duckdb_value_int64(&res, 1, i), *r.opt_val)
          << "opt_val mismatch at " << i;
    } else {
      EXPECT_TRUE(duckdb_value_is_null(&res, 1, i))
          << "opt_val should be null at " << i;
    }

    if (r.name) {
      EXPECT_FALSE(duckdb_value_is_null(&res, 2, i)) << "name null at " << i;
      char* s = duckdb_value_varchar(&res, 2, i);
      EXPECT_STREQ(s, r.name->c_str()) << "name mismatch at " << i;
      duckdb_free(s);
    } else {
      EXPECT_TRUE(duckdb_value_is_null(&res, 2, i))
          << "name should be null at " << i;
    }
  }
  duckdb_destroy_result(&res);

  // Aggregate sanity over the full multi-chunk scan: count of non-null opt_val.
  int64_t expected_non_null = 0;
  for (const auto& r : rows) {
    if (r.opt_val) {
      ++expected_non_null;
    }
  }
  {
    duckdb_result agg;
    ASSERT_EQ(duckdb_query(con, "SELECT count(opt_val) FROM __perfetto_df('t')",
                           &agg),
              DuckDBSuccess)
        << duckdb_result_error(&agg);
    EXPECT_EQ(duckdb_value_int64(&agg, 0, 0), expected_non_null);
    duckdb_destroy_result(&agg);
  }

  duckdb_disconnect(&con);
  duckdb_close(&db);
}

}  // namespace
}  // namespace perfetto::trace_processor::duckdb_integration
