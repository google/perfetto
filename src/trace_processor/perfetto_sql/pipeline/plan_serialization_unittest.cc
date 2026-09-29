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

#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/perfetto_sql/parser/perfetto_sql_parser.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan_test_utils.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/pipeline_sql.h"
#include "src/trace_processor/perfetto_sql/pipeline/test_catalog.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_connection.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::pipeline {
namespace {

using ::testing::HasSubstr;

class PlanSerializationTest : public ::testing::Test {
 protected:
  PlanSerializationTest()
      : connection_(SqliteConnection::CreateConnectionToNewDatabase()),
        catalog_(&pool_, connection_.get()) {
    //   0 (10) -> 1 (20) -> 3 (40)
    //          -> 2 (30)
    catalog_.AddTable(
        "df", {"id", "parent_id", "self"},
        {{3, 1, 40}, {1, 0, 20}, {2, 0, 30}, {0, std::nullopt, 10}});
    Exec("CREATE TABLE spans(ts INTEGER, dur INTEGER, cpu INTEGER)");
    Exec("INSERT INTO spans VALUES (0, 10, 1), (5, 10, 1), (20, 5, 2)");
  }

  void Exec(const std::string& sql) {
    auto statement =
        connection_->PrepareStatement(SqlSource::FromExecuteQuery(sql));
    while (statement.Step()) {
    }
    ASSERT_TRUE(statement.status().ok()) << statement.status().c_message();
  }

  // The plan as it is written into SQL, reading its SQL sources as dataframe
  // arguments, whose dataframes are built into `dataframes_`.
  LogicalPlan Compile(const std::string& sql) {
    PerfettoSqlParser parser(macros_, catalog_, /*pipelines_allowed=*/true);
    parser.Reset(SqlSource::FromExecuteQuery(sql));
    PERFETTO_CHECK(parser.Next());
    LogicalPlan plan = std::move(
        std::get<PerfettoSqlParser::Pipeline>(parser.TakeStatement()).plan);
    auto dataframes = BuildSqlSources(connection_.get(), &pool_, plan);
    PERFETTO_CHECK(dataframes.ok());
    dataframes_ = std::move(*dataframes);
    return MoveSqlSourcesToDataframeArgs(std::move(plan)).plan;
  }

  base::StatusOr<LogicalPlan> RoundTrip(const LogicalPlan& plan) {
    return DeserializePlan(SerializePlan(plan), catalog_);
  }

  StringPool pool_;
  std::unique_ptr<SqliteConnection> connection_;
  TestCatalog catalog_;
  std::vector<std::unique_ptr<dataframe::Dataframe>> dataframes_;
  base::FlatHashMap<std::string, PerfettoSqlParser::Macro> macros_;
};

// Reading numbers columns again, so compare plans up to renumbering.
std::string Renumbered(const LogicalPlan& plan) {
  std::string in = LogicalPlanToString(plan);
  std::string out;
  std::map<std::string, size_t> numbers;
  for (size_t i = 0; i < in.size();) {
    if (in[i] != '#') {
      out += in[i++];
      continue;
    }
    size_t end = in.find_first_not_of("0123456789", i + 1);
    auto [it, inserted] =
        numbers.emplace(in.substr(i, end - i), numbers.size());
    out += "#" + std::to_string(it->second);
    i = end;
  }
  return out;
}

const char* const kPipelines[] = {
    "FROM df |> TREE ACCUMULATE UP SUM(self) AS total |> SELECT id, total",
    "FROM df |> TREE ACCUMULATE DOWN SUM(self) AS a, SUM(id) AS b",
    "FROM (SELECT ts, dur, cpu FROM spans WHERE dur > 1)",
    "INTERVAL INTERSECTION OF (spans AS a, spans AS b) PER cpu",
    "FROM spans |> INTERVAL FLATTEN PER cpu AGGREGATE SUM(dur) AS d",
    "FROM spans |> INTERVAL FLATTEN AGGREGATE COUNT(*) AS n |> SELECT ts, n",
    // Pruning removes the fold, whose node is left behind unread.
    "FROM df |> TREE ACCUMULATE UP SUM(self) AS total |> SELECT id",
};

TEST_F(PlanSerializationTest, EveryOperatorRoundTrips) {
  for (const char* sql : kPipelines) {
    LogicalPlan plan = Compile(sql);
    auto read = RoundTrip(plan);
    ASSERT_TRUE(read.ok()) << sql << ": " << read.status().message();
    EXPECT_EQ(Renumbered(*read), Renumbered(plan)) << sql;
  }
}

TEST_F(PlanSerializationTest, ChangedTablesAreRefused) {
  std::string bytes = SerializePlan(Compile(
      "FROM df |> TREE ACCUMULATE UP SUM(self) AS total |> SELECT id, total"));
  catalog_.RemoveTable("df");
  EXPECT_THAT(DeserializePlan(bytes, catalog_).status().message(),
              HasSubstr("'df' no longer exists"));
  catalog_.AddTable("df", {"id", "parent_id", "other"}, {{0, std::nullopt, 1}});
  EXPECT_THAT(DeserializePlan(bytes, catalog_).status().message(),
              HasSubstr("'df' has changed"));
}

// Anyone can write a plan into SQL, so no bytes may do worse than fail: cut
// short or padded bytes are refused, and corrupt ones are refused or run.
TEST_F(PlanSerializationTest, MalformedPlansAreRefusedOrRun) {
  for (const char* sql : kPipelines) {
    std::string bytes = SerializePlan(Compile(sql));
    for (size_t size = 0; size < bytes.size(); ++size) {
      EXPECT_FALSE(DeserializePlan(bytes.substr(0, size), catalog_).ok())
          << sql << " cut to " << size;
    }
    EXPECT_FALSE(DeserializePlan(bytes + '\0', catalog_).ok()) << sql;
    for (size_t at = 0; at < bytes.size(); ++at) {
      for (char value : {'\0', '\1', '\x7f', '\xff'}) {
        std::string corrupt = bytes;
        corrupt[at] = value;
        auto read = DeserializePlan(corrupt, catalog_);
        if (!read.ok()) {
          continue;
        }
        if (!BindDataframeArgs(*read, DataframeArgs(dataframes_), &pool_)
                 .ok()) {
          continue;
        }
        auto physical = Lower(*read);
        core::exec::RowCursor cursor(physical->source());
        for (bool row = cursor.Open(); row; row = cursor.Next()) {
        }
      }
    }
  }
}

TEST_F(PlanSerializationTest, PlansLoweringCannotRunAreRefused) {
  LogicalPlan plan = Compile(
      "FROM df |> TREE ACCUMULATE UP SUM(self) AS total |> SELECT id, total");
  ASSERT_EQ(plan.nodes.size(), 2u);

  LogicalPlan unknown_column = plan;
  unknown_column.output[0].id = 99;
  EXPECT_FALSE(RoundTrip(unknown_column).ok());

  LogicalPlan reads_nothing_below = plan;
  reads_nothing_below.nodes[1].Cast<op::TreeAccumulate>().node_column =
      reads_nothing_below.nodes[1]
          .Cast<op::TreeAccumulate>()
          .aggregates[0]
          .output;
  EXPECT_FALSE(RoundTrip(reads_nothing_below).ok());
}

}  // namespace
}  // namespace perfetto::trace_processor::pipeline
