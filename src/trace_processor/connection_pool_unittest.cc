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

#include "src/trace_processor/connection_pool.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/util/concurrency_stress_test_util.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

constexpr uint32_t kRowCount = 4000;
constexpr uint32_t kThreadCount = 8;

// Filter + order query whose result we cross-check against a single-threaded
// golden. The filter keeps roughly half the rows and the order is over the
// string column so the comparison exercises both columns.
constexpr char kQuery[] =
    "SELECT v, s FROM t WHERE v >= 1000 ORDER BY s, v";

// A single result row of |kQuery|.
struct Row {
  int64_t v;
  std::string s;
  bool operator==(const Row& o) const { return v == o.v && s == o.s; }
};

// Builds a finalized dataframe with an int column |v| and a string column |s|.
std::unique_ptr<core::dataframe::Dataframe> BuildDataframe(StringPool* pool) {
  core::dataframe::AdhocDataframeBuilder builder({"v", "s"}, pool);
  for (uint32_t i = 0; i < kRowCount; ++i) {
    PERFETTO_CHECK(builder.PushNonNull(0, static_cast<int64_t>(i)));
    std::string s = "row_" + std::to_string(kRowCount - i);
    PERFETTO_CHECK(
        builder.PushNonNull(1, pool->InternString(base::StringView(s))));
  }
  auto df_or = std::move(builder).Build();
  PERFETTO_CHECK(df_or.ok());
  auto df = std::make_unique<core::dataframe::Dataframe>(std::move(*df_or));
  df->Finalize();
  return df;
}

std::vector<Row> RunQuery(PerfettoSqlConnection* conn) {
  auto res =
      conn->ExecuteUntilLastStatement(SqlSource::FromExecuteQuery(kQuery));
  PERFETTO_CHECK(res.ok());
  std::vector<Row> rows;
  if (res->stmt.IsDone())
    return rows;
  sqlite3_stmt* stmt = res->stmt.sqlite_stmt();
  do {
    Row r;
    r.v = sqlite3_column_int64(stmt, 0);
    r.s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    rows.push_back(std::move(r));
  } while (res->stmt.Step());
  PERFETTO_CHECK(res->stmt.status().ok());
  return rows;
}

class ConnectionPoolTest : public ::testing::Test {
 protected:
  StringPool pool_;
  std::unique_ptr<core::dataframe::Dataframe> df_ = BuildDataframe(&pool_);
  std::unique_ptr<PerfettoSqlConnection> primary_ = MakePrimary();

  std::unique_ptr<PerfettoSqlConnection> MakePrimary() {
    auto conn = PerfettoSqlConnection::CreateConnectionToNewDatabase(
        &pool_, /*enable_extra_checks=*/true);
    PerfettoSqlConnection::Initializer init;
    init.static_tables.push_back({df_.get(), "t"});
    conn->Initialize(std::move(init));
    return conn;
  }
};

TEST_F(ConnectionPoolTest, AcquireReleaseRoundTrips) {
  ConnectionPool pool(primary_.get(), &pool_, kThreadCount);

  PerfettoSqlConnection* first;
  {
    ConnectionPool::Handle h = pool.Acquire();
    ASSERT_NE(h.get(), nullptr);
    first = h.get();
    // The forked connection sees the same table as the primary.
    EXPECT_EQ(RunQuery(h.get()), RunQuery(primary_.get()));
  }
  // After release the same connection object should be handed back out.
  ConnectionPool::Handle h2 = pool.Acquire();
  EXPECT_EQ(h2.get(), first);
}

TEST_F(ConnectionPoolTest, DistinctConcurrentAcquirersGetDistinctConnections) {
  ConnectionPool pool(primary_.get(), &pool_, kThreadCount);

  std::mutex mutex;
  std::set<PerfettoSqlConnection*> seen;
  std::vector<ConnectionPool::Handle> handles;
  for (uint32_t i = 0; i < kThreadCount; ++i) {
    ConnectionPool::Handle h = pool.Acquire();
    {
      std::lock_guard<std::mutex> lock(mutex);
      EXPECT_TRUE(seen.insert(h.get()).second) << "duplicate connection";
    }
    handles.push_back(std::move(h));
  }
  EXPECT_EQ(seen.size(), kThreadCount);
}

TEST_F(ConnectionPoolTest, ConcurrentQueriesMatchGolden) {
  std::vector<Row> golden = RunQuery(primary_.get());
  ASSERT_FALSE(golden.empty());

  ConnectionPool pool(primary_.get(), &pool_, kThreadCount);
  ConcurrentlyRun(kThreadCount, /*iterations=*/16, [&](uint32_t) {
    ConnectionPool::Handle h = pool.Acquire();
    std::vector<Row> rows = RunQuery(h.get());
    EXPECT_EQ(rows, golden);
  });
}

}  // namespace
}  // namespace perfetto::trace_processor
