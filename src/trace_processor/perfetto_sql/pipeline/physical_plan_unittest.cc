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

#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/perfetto_sql/parser/perfetto_sql_parser.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/test_catalog.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_connection.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::pipeline {
namespace {

using core::exec::ColumnView;
using core::exec::RowCursor;
using core::exec::Variant;
using testing::ElementsAre;
using testing::HasSubstr;
using testing::Pair;

// Reads an integer cell from a flat or variant column.
std::optional<int64_t> IntAt(const RowCursor& cursor, uint32_t column) {
  const ColumnView& view = cursor.batch().column(column);
  if (view.kind() == ColumnView::Kind::kVariant) {
    Variant cell = cursor.Value<Variant>(column);
    if (cell.type == Variant::Type::kNull) {
      return std::nullopt;
    }
    return cell.AsInt64();
  }
  const core::BitVector* validity = view.validity();
  uint32_t row = cursor.row();
  if (validity && !validity->is_set(view.selection().GetIndex(row))) {
    return std::nullopt;
  }
  core::StorageType type = view.type();
  if (type.Is<core::Id>() || type.Is<core::Uint32>()) {
    return cursor.Value<uint32_t>(column);
  }
  if (type.Is<core::Int32>()) {
    return cursor.Value<int32_t>(column);
  }
  return cursor.Value<int64_t>(column);
}

class PhysicalPlanTest : public ::testing::Test {
 protected:
  PhysicalPlanTest()
      : connection_(SqliteConnection::CreateConnectionToNewDatabase()),
        catalog_(&pool_, connection_.get()) {
    env_.connection = connection_.get();
    env_.pool = &pool_;
  }

  void Exec(const std::string& sql) {
    auto statement =
        connection_->PrepareStatement(SqlSource::FromExecuteQuery(sql));
    while (statement.Step()) {
    }
    ASSERT_TRUE(statement.status().ok()) << statement.status().c_message();
  }

  // Four nodes, inserted parent first:
  //   0 (10) -> 1 (20) -> 3 (40)
  //          -> 2 (30)
  void CreateTree() {
    Exec("CREATE TABLE tree(id INTEGER, parent_id INTEGER, self INTEGER)");
    Exec(
        "INSERT INTO tree VALUES (0, NULL, 10), (1, 0, 20), (2, 0, 30), "
        "(3, 1, 40)");
  }

  // Same tree as a dataframe, child first.
  void CreateDataframeTree() {
    catalog_.AddTable(
        "df", {"id", "parent_id", "self"},
        {{3, 1, 40}, {1, 0, 20}, {2, 0, 30}, {0, std::nullopt, 10}});
  }

  base::StatusOr<std::unique_ptr<PhysicalPlan>> Plan(const std::string& sql) {
    PerfettoSqlParser parser(macros_, catalog_,
                             /*pipelines_allowed=*/true);
    parser.Reset(SqlSource::FromExecuteQuery(sql));
    if (!parser.Next()) {
      return parser.status();
    }
    const auto* pipeline =
        std::get_if<PerfettoSqlParser::Pipeline>(&parser.statement());
    PERFETTO_CHECK(pipeline);
    return Lower(pipeline->plan, env_);
  }

  std::vector<std::string> Names(const PhysicalPlan& plan) {
    std::vector<std::string> names;
    for (const PhysicalPlan::Column& column : plan.columns()) {
      names.push_back(column.name);
    }
    return names;
  }

  // Runs `plan` and returns (id, value) pairs sorted by id.
  using Rows = std::vector<std::pair<int64_t, std::optional<int64_t>>>;
  base::StatusOr<Rows> Run(const PhysicalPlan& plan, const std::string& value) {
    uint32_t id_column = 0;
    uint32_t value_column = 0;
    for (const PhysicalPlan::Column& column : plan.columns()) {
      if (column.name == "id") {
        id_column = column.index;
      } else if (column.name == value) {
        value_column = column.index;
      }
    }
    Rows out;
    RowCursor cursor(plan.source());
    for (bool row = cursor.Open(); row; row = cursor.Next()) {
      out.emplace_back(*IntAt(cursor, id_column), IntAt(cursor, value_column));
    }
    if (!cursor.status().ok()) {
      return cursor.status();
    }
    std::sort(out.begin(), out.end());
    return out;
  }

  StringPool pool_;
  std::unique_ptr<SqliteConnection> connection_;
  TestCatalog catalog_;
  LowerEnvironment env_;
  base::FlatHashMap<std::string, PerfettoSqlParser::Macro> macros_;
};

TEST_F(PhysicalPlanTest, AFromAloneReadsTheSource) {
  CreateTree();
  auto plan = Plan("FROM tree");
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  EXPECT_THAT(Names(**plan), ElementsAre("id", "parent_id", "self"));
  auto rows = Run(**plan, "self");
  ASSERT_TRUE(rows.ok()) << rows.status().message();
  EXPECT_THAT(*rows,
              ElementsAre(Pair(0, 10), Pair(1, 20), Pair(2, 30), Pair(3, 40)));
}

TEST_F(PhysicalPlanTest, AccumulateUpSumsEachSubtree) {
  CreateTree();
  auto plan = Plan("FROM tree |> TREE ACCUMULATE UP SUM(self) AS total");
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  EXPECT_THAT(Names(**plan), ElementsAre("id", "parent_id", "self", "total"));
  auto rows = Run(**plan, "total");
  ASSERT_TRUE(rows.ok()) << rows.status().message();
  EXPECT_THAT(*rows,
              ElementsAre(Pair(0, 100), Pair(1, 60), Pair(2, 30), Pair(3, 40)));
}

TEST_F(PhysicalPlanTest, AccumulateDownSumsEachRootPath) {
  CreateTree();
  auto plan = Plan("FROM tree |> TREE ACCUMULATE DOWN SUM(self) AS path");
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  auto rows = Run(**plan, "path");
  ASSERT_TRUE(rows.ok()) << rows.status().message();
  EXPECT_THAT(*rows,
              ElementsAre(Pair(0, 10), Pair(1, 30), Pair(2, 40), Pair(3, 70)));
}

TEST_F(PhysicalPlanTest, TheSourceCanBeArbitrarySql) {
  CreateTree();
  auto plan = Plan(
      "FROM (SELECT id, parent_id, self * 2 AS doubled FROM tree) "
      "|> TREE ACCUMULATE UP SUM(doubled) AS total");
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  auto rows = Run(**plan, "total");
  ASSERT_TRUE(rows.ok()) << rows.status().message();
  EXPECT_THAT(
      *rows, ElementsAre(Pair(0, 200), Pair(1, 120), Pair(2, 60), Pair(3, 80)));
}

TEST_F(PhysicalPlanTest, StagesAndAggregatesCompose) {
  CreateTree();
  Exec("CREATE VIEW sized AS SELECT *, 1 AS one FROM tree");
  auto plan = Plan(
      "FROM sized\n"
      "|> TREE ACCUMULATE UP SUM(self) AS total, SUM(one) AS size\n"
      "|> TREE ACCUMULATE DOWN SUM(size) AS path_size");
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  EXPECT_THAT(Names(**plan), ElementsAre("id", "parent_id", "self", "one",
                                         "total", "size", "path_size"));
  auto totals = Run(**plan, "total");
  ASSERT_TRUE(totals.ok()) << totals.status().message();
  EXPECT_THAT(*totals,
              ElementsAre(Pair(0, 100), Pair(1, 60), Pair(2, 30), Pair(3, 40)));
  auto sizes = Run(**plan, "size");
  ASSERT_TRUE(sizes.ok()) << sizes.status().message();
  EXPECT_THAT(*sizes,
              ElementsAre(Pair(0, 4), Pair(1, 2), Pair(2, 1), Pair(3, 1)));
  auto path_sizes = Run(**plan, "path_size");
  ASSERT_TRUE(path_sizes.ok()) << path_sizes.status().message();
  EXPECT_THAT(*path_sizes,
              ElementsAre(Pair(0, 4), Pair(1, 6), Pair(2, 5), Pair(3, 7)));
}

TEST_F(PhysicalPlanTest, ConsecutiveFoldsReuseTreeColumns) {
  CreateTree();
  auto plan = Plan(
      "FROM tree |> TREE ACCUMULATE DOWN SUM(self) AS path "
      "|> TREE ACCUMULATE DOWN SUM(path) AS path2 "
      "|> TREE ACCUMULATE UP SUM(path2) AS total");
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  // Three source columns, two physical tree columns, then three results.
  // Numbering is reused even when the fold direction changes.
  EXPECT_EQ((*plan)->columns().back().index, 7u);
  auto rows = Run(**plan, "total");
  ASSERT_TRUE(rows.ok()) << rows.status().message();
  EXPECT_THAT(*rows, ElementsAre(Pair(0, 210), Pair(1, 150), Pair(2, 50),
                                 Pair(3, 110)));
}

TEST_F(PhysicalPlanTest, OutputBindingsUseIdsRatherThanBatchPositions) {
  CreateTree();
  PerfettoSqlParser parser(macros_, catalog_,
                           /*pipelines_allowed=*/true);
  parser.Reset(SqlSource::FromExecuteQuery(
      "FROM tree |> TREE ACCUMULATE DOWN SUM(self) AS path "
      "|> TREE ACCUMULATE UP SUM(path) AS total"));
  ASSERT_TRUE(parser.Next()) << parser.status().message();
  auto logical = std::get<PerfettoSqlParser::Pipeline>(parser.statement()).plan;
  ColumnId total = logical.output.back().id;
  ColumnId id = logical.output.front().id;
  // Project and alias the same value twice, independently of the source names.
  logical.output = {{"total", total}, {"id", id}, {"again", total}};
  auto plan = Lower(logical, env_);
  EXPECT_THAT(Names(*plan), ElementsAre("total", "id", "again"));
  EXPECT_NE(plan->columns()[0].index, total);
  EXPECT_EQ(plan->columns()[0].index, plan->columns()[2].index);
  auto rows = Run(*plan, "total");
  ASSERT_TRUE(rows.ok()) << rows.status().message();
  EXPECT_THAT(
      *rows, ElementsAre(Pair(0, 150), Pair(1, 100), Pair(2, 40), Pair(3, 70)));
}

// SQLite has no `df` table, so this only works if the dataframe is scanned
// directly.
TEST_F(PhysicalPlanTest, ADataframeIsScannedDirectly) {
  CreateDataframeTree();
  auto plan = Plan("FROM df |> TREE ACCUMULATE UP SUM(self) AS total");
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  EXPECT_THAT(Names(**plan), ElementsAre("id", "parent_id", "self", "total"));
  auto rows = Run(**plan, "total");
  ASSERT_TRUE(rows.ok()) << rows.status().message();
  EXPECT_THAT(*rows,
              ElementsAre(Pair(0, 100), Pair(1, 60), Pair(2, 30), Pair(3, 40)));
}

// The scan shares the dataframe's columns, so dropping the table does not
// affect a plan already lowered.
TEST_F(PhysicalPlanTest, APlanOutlivesTheTableItReads) {
  CreateDataframeTree();
  auto plan = Plan("FROM df |> TREE ACCUMULATE UP SUM(self) AS total");
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  catalog_.RemoveTable("df");
  auto rows = Run(**plan, "total");
  ASSERT_TRUE(rows.ok()) << rows.status().message();
  EXPECT_THAT(*rows,
              ElementsAre(Pair(0, 100), Pair(1, 60), Pair(2, 30), Pair(3, 40)));
}

TEST_F(PhysicalPlanTest, LogicalPlanRetainsColumnsBeforeLowering) {
  CreateDataframeTree();
  PerfettoSqlParser parser(macros_, catalog_,
                           /*pipelines_allowed=*/true);
  parser.Reset(SqlSource::FromExecuteQuery(
      "FROM df |> TREE ACCUMULATE UP SUM(self) AS total"));
  ASSERT_TRUE(parser.Next());
  LogicalPlan plan =
      std::get<PerfettoSqlParser::Pipeline>(parser.statement()).plan;
  catalog_.RemoveTable("df");
  auto physical = Lower(plan, env_);
  auto rows = Run(*physical, "total");
  ASSERT_TRUE(rows.ok()) << rows.status().message();
  EXPECT_THAT(*rows,
              ElementsAre(Pair(0, 100), Pair(1, 60), Pair(2, 30), Pair(3, 40)));
}

TEST_F(PhysicalPlanTest, ValuesWhichAreNotIntegersFailTheRun) {
  CreateTree();
  Exec("INSERT INTO tree VALUES (4, 0, 'many')");
  auto plan = Plan("FROM tree |> TREE ACCUMULATE UP SUM(self) AS total");
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  EXPECT_THAT(Run(**plan, "total").status().message(), HasSubstr("'self'"));
}

TEST_F(PhysicalPlanTest, DiagnosticsUseDefiningNamesAfterProjection) {
  CreateTree();
  Exec("INSERT INTO tree VALUES (4, 0, 'many')");
  PerfettoSqlParser parser(macros_, catalog_,
                           /*pipelines_allowed=*/true);
  parser.Reset(SqlSource::FromExecuteQuery(
      "FROM tree |> TREE ACCUMULATE UP SUM(self) AS total"));
  ASSERT_TRUE(parser.Next()) << parser.status().message();
  auto logical = std::get<PerfettoSqlParser::Pipeline>(parser.statement()).plan;
  // Neither the original input name nor its value is exposed in the result.
  logical.output = {{"id", logical.output.front().id},
                    {"renamed_total", logical.output.back().id}};
  auto physical = Lower(logical, env_);
  EXPECT_THAT(Run(*physical, "renamed_total").status().message(),
              HasSubstr("'self'"));
}

TEST_F(PhysicalPlanTest, AnIncompleteTreeFailsTheRun) {
  CreateTree();
  auto plan = Plan(
      "FROM (SELECT * FROM tree WHERE id != 1) "
      "|> TREE ACCUMULATE UP SUM(self) AS total");
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  EXPECT_FALSE(Run(**plan, "total").ok());
}

TEST_F(PhysicalPlanTest, APlanCanRunMoreThanOnce) {
  CreateTree();
  auto plan = Plan("FROM tree |> TREE ACCUMULATE UP SUM(self) AS total");
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  auto first = Run(**plan, "total");
  auto second = Run(**plan, "total");
  ASSERT_TRUE(first.ok() && second.ok());
  EXPECT_EQ(*first, *second);
}

}  // namespace
}  // namespace perfetto::trace_processor::pipeline
