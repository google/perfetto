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
#include "src/trace_processor/perfetto_sql/pipeline/operation_registry.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/pipeline_sql.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_key.h"
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
    auto dataframes =
        TestCatalog::BuildSqlSources(connection_.get(), &pool_, plan);
    PERFETTO_CHECK(dataframes.ok());
    dataframes_ = std::move(*dataframes);
    return MoveSqlSourcesToDataframeArgs(std::move(plan)).plan;
  }

  std::string Serialize(const std::string& sql) {
    return SerializePlan(Compile(sql), key_);
  }

  base::StatusOr<LogicalPlan> Deserialize(const std::string& bytes) {
    return DeserializePlan(bytes, key_, catalog_);
  }

  PlanKey key_{1, 2};
  StringPool pool_;
  std::unique_ptr<SqliteConnection> connection_;
  TestCatalog catalog_;
  std::vector<std::unique_ptr<dataframe::Dataframe>> dataframes_;
  base::FlatHashMap<std::string, PerfettoSqlParser::Macro> macros_;
};

const char* const kPipelines[] = {
    "FROM df |> TREE ACCUMULATE UP SUM(self) AS total |> SELECT id, total",
    "FROM df |> TREE ACCUMULATE DOWN SUM(self) AS a, SUM(id) AS b",
    "FROM (SELECT ts, dur, cpu FROM spans WHERE dur > 1)",
    "INTERVAL INTERSECTION OF (spans AS a, spans AS b) PER cpu",
    "FROM spans |> INTERVAL FLATTEN PER cpu AGGREGATE SUM(dur) AS d",
    "FROM spans |> INTERVAL FLATTEN AGGREGATE COUNT(*) AS n |> SELECT ts, n",
    "FROM spans |> INTERVAL FILL GAPS WITH spans PER cpu",
    "FROM spans |> INTERVAL FILL GAPS WITH (SELECT ts, dur FROM spans)",
    "FROM df |> ORDER BY self DESC, id |> SELECT id",
    "FROM spans |> INTERVAL FLATTEN AGGREGATE COUNT(*) AS n |> ORDER BY n",
    "FROM df |> SELECT parent_id |> DISTINCT",
    "FROM df |> ORDER BY self |> LIMIT 3 OFFSET 1 |> SELECT id",
    "FROM df |> WHERE self > 0 AND id < 10",
    "FROM df |> WHERE self != 'a''b' AND id IN (1, 2.5, 'x') AND id NOTNULL",
    "FROM spans |> AGGREGATE COUNT(*) AS n, SUM(dur) AS d GROUP BY cpu AS c",
    "FROM spans |> AGGREGATE MAX(dur) AS m |> SELECT m",
    R"(GRAPH DOMINATOR TREE NODES df EDGES (SELECT parent_id AS src, id AS dst
      FROM df) FROM (SELECT 0 AS id) |> TREE ACCUMULATE UP SUM(self) AS t)",
    // Pruning removes the fold, whose node is left behind unread.
    "FROM df |> TREE ACCUMULATE UP SUM(self) AS total |> SELECT id",
};

TEST_F(PlanSerializationTest, PlansRoundTrip) {
  for (const char* sql : kPipelines) {
    auto read = Deserialize(Serialize(sql));
    ASSERT_TRUE(read.ok()) << sql << ": " << read.status().message();
    ASSERT_TRUE(
        BindDataframeArgs(*read, DataframeArgs(dataframes_), &pool_).ok())
        << sql;
    EXPECT_TRUE(Lower(*read, {&pool_}).ok()) << sql;
  }
}

TEST_F(PlanSerializationTest, ChangedTablesAreRefused) {
  std::string bytes = Serialize(
      "FROM df |> TREE ACCUMULATE UP SUM(self) AS total |> SELECT id, total");
  catalog_.RemoveTable("df");
  EXPECT_THAT(Deserialize(bytes).status().message(),
              HasSubstr("'df' no longer exists"));
  catalog_.AddTable("df", {"id", "parent_id", "other"}, {{0, std::nullopt, 1}});
  EXPECT_THAT(Deserialize(bytes).status().message(),
              HasSubstr("'df' has changed"));
  catalog_.RemoveTable("df");
  catalog_.AddTable("df", {"id", "parent_id"}, {{0, std::nullopt}});
  EXPECT_THAT(Deserialize(bytes).status().message(),
              HasSubstr("'df' has changed"));
}

// A table made again as it was is read as before, as a view persists.
TEST_F(PlanSerializationTest, RemadeTablesAreRead) {
  std::string bytes = Serialize("FROM df |> SELECT id, self");
  catalog_.RemoveTable("df");
  catalog_.AddTable("df", {"id", "parent_id", "self"},
                    {{1, 0, 5}, {0, std::nullopt, 6}});
  EXPECT_TRUE(Deserialize(bytes).ok());
}

// Anyone can write bytes into SQL, so only bytes the key stamped are read.
TEST_F(PlanSerializationTest, UnstampedPlansAreRefused) {
  for (const char* sql : kPipelines) {
    std::string bytes = Serialize(sql);
    for (size_t size = 0; size < bytes.size(); ++size) {
      EXPECT_FALSE(Deserialize(bytes.substr(0, size)).ok())
          << sql << " cut to " << size;
    }
    EXPECT_FALSE(Deserialize(bytes + '\0').ok()) << sql;
    for (size_t at = 0; at < bytes.size(); ++at) {
      std::string corrupt = bytes;
      corrupt[at] = static_cast<char>(corrupt[at] ^ 1);
      EXPECT_FALSE(Deserialize(corrupt).ok()) << sql << " at " << at;
    }
    EXPECT_FALSE(DeserializePlan(bytes, PlanKey(1, 3), catalog_).ok()) << sql;
  }
}

TEST_F(PlanSerializationTest, PlansWithTooManyOutputsAreNotWritten) {
  LogicalPlan plan = Compile("FROM df |> SELECT id");
  ASSERT_EQ(plan.output().size(), 1u);

  LogicalPlan at_limit = plan;
  at_limit.output().assign(kMaxPipelineColumns, plan.output()[0]);
  EXPECT_TRUE(SelectPipelineSql(at_limit, key_).ok());

  LogicalPlan over_limit = plan;
  over_limit.output().assign(kMaxPipelineColumns + 1, plan.output()[0]);
  EXPECT_FALSE(SelectPipelineSql(over_limit, key_).ok());
}

// A lookup checks no two operations share the tag it finds.
TEST(PlanSerializationTagTest, TagsAreUnique) {
  for (uint32_t tag = 0; tag < 256; ++tag) {
    FindOperationByTag(static_cast<uint8_t>(tag));
  }
}

}  // namespace
}  // namespace perfetto::trace_processor::pipeline
