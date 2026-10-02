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

#include "src/trace_processor/perfetto_sql/pipeline/cost_estimation.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/perfetto_sql/parser/perfetto_sql_parser.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/pipeline_sql.h"
#include "src/trace_processor/perfetto_sql/pipeline/test_catalog.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_connection.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::pipeline {
namespace {

class CostEstimationTest : public ::testing::Test {
 protected:
  CostEstimationTest()
      : connection_(SqliteConnection::CreateConnectionToNewDatabase()),
        catalog_(&pool_, connection_.get()) {
    // 1000 spans: a unique sorted id, a timestamp and 4 cpus.
    std::vector<std::vector<std::optional<int64_t>>> rows;
    for (int64_t i = 0; i < 1000; ++i) {
      rows.push_back({i, i * 10, 5, i % 4});
    }
    catalog_.AddTable("spans", {"id", "ts", "dur", "cpu"}, rows);
    auto statement = connection_->PrepareStatement(
        SqlSource::FromExecuteQuery("CREATE TABLE sql_spans(ts, dur)"));
    PERFETTO_CHECK(!statement.Step() && statement.status().ok());
  }

  // Estimates the plan as it is written into SQL.
  PlanEstimate Estimate(const std::string& sql) {
    PerfettoSqlParser parser(macros_, catalog_, /*pipelines_allowed=*/true);
    parser.Reset(SqlSource::FromExecuteQuery(sql));
    PERFETTO_CHECK(parser.Next());
    LogicalPlan plan = std::move(
        std::get<PerfettoSqlParser::Pipeline>(parser.TakeStatement()).plan);
    return EstimatePlan(MoveSqlSourcesToDataframeArgs(std::move(plan)).plan,
                        catalog_);
  }

  StringPool pool_;
  std::unique_ptr<SqliteConnection> connection_;
  TestCatalog catalog_;
  base::FlatHashMap<std::string, PerfettoSqlParser::Macro> macros_;
};

TEST_F(CostEstimationTest, EstimatesAsTheDataframesWould) {
  PlanEstimate all = Estimate("FROM spans");
  EXPECT_EQ(all.rows.estimated, 1000u);
  EXPECT_GT(all.cost, 0);

  // A unique column's equality keeps one row, and costs less to find than
  // reading everything, as the dataframe plans it.
  PlanEstimate one = Estimate("FROM spans |> WHERE id = 5");
  EXPECT_EQ(one.rows.estimated, 1u);
  EXPECT_LT(one.cost, all.cost);
  EXPECT_LT(Estimate("FROM spans |> WHERE ts > 5").rows.estimated, 1000u);

  // SQL is only built into a dataframe when the query runs, so its size is
  // assumed, and assumed large.
  EXPECT_EQ(Estimate("FROM sql_spans").rows.estimated, 100000u);
}

TEST_F(CostEstimationTest, EstimatesEachOperator) {
  PlanEstimate scan = Estimate("FROM spans");
  PlanEstimate fold = Estimate(
      "FROM spans |> SELECT id, id AS parent_id, dur "
      "|> TREE ACCUMULATE UP SUM(dur) AS total");
  EXPECT_EQ(fold.rows.estimated, 1000u);
  EXPECT_GT(fold.cost, scan.cost);

  // A filter which cannot move below the fold keeps half the rows, as the
  // dataframe planner assumes of a range it knows nothing about.
  PlanEstimate filtered = Estimate(
      "FROM spans |> SELECT id, id AS parent_id, dur "
      "|> TREE ACCUMULATE UP SUM(dur) AS total |> WHERE total > 5");
  EXPECT_EQ(filtered.rows.estimated, 500u);

  // Intersecting produces about one region per row swept.
  PlanEstimate isect =
      Estimate("INTERVAL INTERSECTION OF (spans AS a, spans AS b) PER cpu");
  EXPECT_EQ(isect.rows.estimated, 2000u);
  EXPECT_GT(isect.cost, 2 * scan.cost);
}

}  // namespace
}  // namespace perfetto::trace_processor::pipeline
