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
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan_test_utils.h"

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
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/perfetto_sql/parser/perfetto_sql_parser.h"
#include "src/trace_processor/perfetto_sql/pipeline/column_pruning.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/pipeline_sql.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"
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
  if (!view.IsValid(cursor.batch_row())) {
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
  using Rows = std::vector<std::pair<int64_t, std::optional<int64_t>>>;

  PhysicalPlanTest()
      : connection_(SqliteConnection::CreateConnectionToNewDatabase()),
        catalog_(&pool_, connection_.get()) {}

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

  base::StatusOr<LogicalPlan> Compile(const std::string& sql) {
    PerfettoSqlParser parser(macros_, catalog_,
                             /*pipelines_allowed=*/true);
    parser.Reset(SqlSource::FromExecuteQuery(sql));
    if (!parser.Next()) {
      return parser.status();
    }
    PERFETTO_CHECK(std::holds_alternative<PerfettoSqlParser::Pipeline>(
        parser.statement()));
    return std::move(
        std::get<PerfettoSqlParser::Pipeline>(parser.TakeStatement()).plan);
  }

  base::StatusOr<std::unique_ptr<PhysicalPlan>> Plan(const std::string& sql) {
    ASSIGN_OR_RETURN(LogicalPlan plan, Compile(sql));
    return LowerReadingSql(std::move(plan));
  }

  // Lowers `plan` to read each of its SQL sources as a dataframe, built up
  // front as SQLite builds it where the pipeline is written.
  base::StatusOr<std::unique_ptr<PhysicalPlan>> LowerReadingSql(
      LogicalPlan plan) {
    ASSIGN_OR_RETURN(auto dataframes, TestCatalog::BuildSqlSources(
                                          connection_.get(), &pool_, plan));
    LogicalPlan moved = MoveSqlSourcesToDataframeArgs(std::move(plan)).plan;
    RETURN_IF_ERROR(
        BindDataframeArgs(moved, DataframeArgs(dataframes), &pool_));
    return Lower(moved);
  }

  std::vector<std::string> Names(const PhysicalPlan& plan) {
    std::vector<std::string> names;
    for (const PhysicalPlan::Column& column : plan.columns()) {
      names.push_back(column.name);
    }
    return names;
  }

  // Runs `plan` and returns (id, value) pairs sorted by id.
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
  base::FlatHashMap<std::string, PerfettoSqlParser::Macro> macros_;
};

// Optimizing a copy must not change the results of the original plan.
TEST_F(PhysicalPlanTest, PruningACopyLeavesTheOriginalResultsIntact) {
  CreateDataframeTree();
  auto original = Compile(
      "FROM df |> TREE ACCUMULATE UP SUM(self) AS total |> SELECT id, total");
  ASSERT_TRUE(original.ok()) << original.status().message();

  LogicalPlan copy = *original;
  copy.output().resize(1);
  PruneColumns(copy);

  // Copy assignment must also preserve independent operation payloads.
  copy = *original;
  copy.output().resize(1);
  PruneColumns(copy);

  auto physical = Lower(*original);
  ASSERT_TRUE(physical.ok()) << physical.status().message();
  auto rows = Run(**physical, "total");
  ASSERT_TRUE(rows.ok()) << rows.status().message();
  EXPECT_THAT(*rows,
              ElementsAre(Pair(0, 100), Pair(1, 60), Pair(2, 30), Pair(3, 40)));
}

TEST_F(PhysicalPlanTest, ConsecutiveFoldsReuseTreeColumns) {
  CreateTree();
  auto plan = Plan(
      "FROM tree |> TREE ACCUMULATE DOWN SUM(self) AS path "
      "|> TREE ACCUMULATE DOWN SUM(path) AS path2 "
      "|> TREE ACCUMULATE UP SUM(path2) AS total");
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  // The tree is numbered once. Both downward folds share one parent-first
  // walk. A new walk is added only when the direction changes. The
  // parent-first walk can output small batches, so they are combined before
  // the child-first walk.
  EXPECT_EQ(PhysicalPlanToString(**plan),
            "main: DataframeScan | TreeNumberNodes(#0, #1) | "
            "TreeParentFirst(#3, #4) | AssertType(#2 to Int64) | "
            "TreeAccumulateDown(#3, #4; SUM(#2)) | "
            "TreeAccumulateDown(#3, #4; SUM(#5)) | CombineBatches | "
            "TreeChildFirst(#3, #4) | "
            "TreeAccumulateUp(#3, #4; SUM(#6))");
  auto rows = Run(**plan, "total");
  ASSERT_TRUE(rows.ok()) << rows.status().message();
  EXPECT_THAT(*rows, ElementsAre(Pair(0, 210), Pair(1, 150), Pair(2, 50),
                                 Pair(3, 110)));
}

// A requirement adds an operator only if the rows do not already meet it.
TEST_F(PhysicalPlanTest, RowsAlreadyInOrderAreNotSortedAgain) {
  Exec("CREATE TABLE t(ts INTEGER, dur INTEGER, cpu INTEGER)");
  Exec("INSERT INTO t VALUES (5, 10, 1), (0, 10, 0), (7, 1, 1), (2, 1, 0)");
  auto plan = [&](const std::string& sql) -> std::string {
    auto lowered = Plan(sql);
    EXPECT_TRUE(lowered.ok()) << lowered.status().message();
    return lowered.ok() ? PhysicalPlanToString(**lowered) : "";
  };
  // Rows sorted by (cpu, ts) are also sorted by cpu. A sort that nothing
  // depends on yet is dropped when the rows are sorted again.
  EXPECT_EQ(plan("FROM t |> ORDER BY cpu, ts |> ORDER BY cpu"),
            "main: DataframeScan | Sort(#2, #0)");
  EXPECT_EQ(plan("FROM t |> ORDER BY cpu, ts |> ORDER BY ts"),
            "main: DataframeScan | Sort(#0)");
  EXPECT_EQ(plan("FROM t |> ORDER BY cpu |> INTERVAL FLATTEN PER cpu "
                 "AGGREGATE COUNT(*) AS n"),
            "main: DataframeScan | AssertType(#0 to Int64) | "
            "AssertType(#1 to Int64) | GroupedSort(#2; #0) | "
            "IntervalFlatten(#0, #1, keys: #2, groups: #3; COUNT(*))");

  // The scan outputs rows sorted by ts. Grouping is stable, so grouping by
  // cpu is enough to sort by ts within each cpu. No sort is needed.
  Exec("CREATE TABLE by_ts(ts INTEGER, dur INTEGER, cpu INTEGER)");
  Exec("INSERT INTO by_ts VALUES (0, 1, 1), (2, 1, 0), (5, 1, 1)");
  EXPECT_EQ(plan("FROM by_ts |> INTERVAL FLATTEN PER cpu "
                 "AGGREGATE COUNT(*) AS n"),
            "main: DataframeScan | AssertType(#0 to Int64) | "
            "AssertType(#1 to Int64) | GroupedSort(#2) | "
            "IntervalFlatten(#0, #1, keys: #2, groups: #3; COUNT(*))");

  // Groups are output in the order first seen, so if the input is sorted by
  // the key, the groups are too.
  EXPECT_EQ(
      plan("FROM t |> ORDER BY cpu |> AGGREGATE COUNT(*) AS n GROUP BY "
           "cpu |> ORDER BY cpu"),
      "main: DataframeScan | Sort(#0) | HashAggregate(keys: #0; COUNT(*))");
  EXPECT_EQ(
      plan("FROM t |> ORDER BY ts |> AGGREGATE COUNT(*) AS n GROUP BY "
           "cpu |> ORDER BY cpu"),
      "main: DataframeScan | Sort(#0) | HashAggregate(keys: #1; COUNT(*)) | "
      "Sort(#0)");
}

// Small batches are combined only after a step that may output them, and only
// before an operator that works best on full batches.
TEST_F(PhysicalPlanTest, OnlySmallBatchesAreCombined) {
  Exec("CREATE TABLE t(ts INTEGER, dur INTEGER, n INTEGER)");
  Exec("INSERT INTO t VALUES (5, 10, 2), (0, 10, 1)");
  auto plan = [&](const std::string& sql) -> std::string {
    auto lowered = Plan(sql);
    EXPECT_TRUE(lowered.ok()) << lowered.status().message();
    return lowered.ok() ? PhysicalPlanToString(**lowered) : "";
  };
  EXPECT_EQ(plan("FROM t |> ORDER BY n"), "main: DataframeScan | Sort(#2)");
  EXPECT_EQ(plan("FROM t |> INTERVAL FLATTEN AGGREGATE COUNT(*) AS c "
                 "|> ORDER BY c"),
            "main: DataframeScan | AssertType(#0 to Int64) | "
            "AssertType(#1 to Int64) | Sort(#0) | "
            "IntervalFlatten(#0, #1; COUNT(*)) | CombineBatches | Sort(#2)");
}

// Each operand is grouped by its PER keys and sorted by ts within each group,
// unless it already is.
TEST_F(PhysicalPlanTest, IntersectionOrdersItsOperands) {
  Exec("CREATE TABLE a(ts INTEGER, dur INTEGER, cpu INTEGER)");
  Exec("INSERT INTO a VALUES (5, 10, 1), (0, 10, 0)");
  Exec("CREATE TABLE b(ts INTEGER, dur INTEGER, cpu INTEGER)");
  Exec("INSERT INTO b VALUES (7, 10, 0), (2, 10, 1)");
  auto plan = Plan(
      "INTERVAL INTERSECTION OF (a AS p, b AS q) PER cpu "
      "|> SELECT ts, dur, p.cpu");
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  // ts and dur are converted to Int64. So is cpu, because it has a different
  // integer type in each dataframe.
  EXPECT_EQ(PhysicalPlanToString(**plan),
            "side: DataframeScan | AssertType(#0 to Int64) | "
            "AssertType(#1 to Int64) | AssertType(#2 to Int64) | "
            "GroupedSort(#2; #0)\n"
            "side: DataframeScan | AssertType(#0 to Int64) | "
            "AssertType(#1 to Int64) | AssertType(#2 to Int64) | "
            "GroupedSort(#2; #0)\n"
            "main: IntervalIntersect(#0, #1, keys: #2, groups: #3; #0, #1, "
            "keys: #2, groups: #3)");
}

// An intersection needs its operands' bounds and PER columns, but only passes
// on the columns something after it reads: it neither buffers nor gathers
// the rest.
TEST_F(PhysicalPlanTest, IntersectionCarriesOnlyColumnsUsedAfterIt) {
  Exec("CREATE TABLE a(ts INTEGER, dur INTEGER, cpu INTEGER, x INTEGER)");
  Exec("INSERT INTO a VALUES (0, 10, 1, 7)");
  Exec("CREATE TABLE b(ts INTEGER, dur INTEGER, cpu INTEGER, y INTEGER)");
  Exec("INSERT INTO b VALUES (5, 10, 1, 8)");
  auto plan = Plan(
      "INTERVAL INTERSECTION OF (a AS p, b AS q) PER cpu "
      "|> SELECT ts, dur, q.y");
  ASSERT_TRUE(plan.ok()) << plan.status().message();

  const core::exec::Source& source = (*plan)->source();
  core::exec::Context context;
  auto state = source.MakeState(context);
  core::exec::RowBatch batch;
  ASSERT_TRUE(source.GetData(batch, *state));
  // The region's ts and dur, and q.y.
  EXPECT_EQ(batch.column_count(), 3u);

  RowCursor cursor(source);
  ASSERT_TRUE(cursor.Open());
  std::vector<std::optional<int64_t>> row;
  for (const PhysicalPlan::Column& column : (*plan)->columns()) {
    row.push_back(IntAt(cursor, column.index));
  }
  EXPECT_THAT(row, ElementsAre(5, 5, 8));
  EXPECT_FALSE(cursor.Next());
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
  ColumnId total = logical.output().back().id;
  ColumnId id = logical.output().front().id;
  // Project and alias the same value twice, independently of the source names.
  logical.output() = {{"total", total}, {"id", id}, {"again", total}};
  auto plan = std::move(*LowerReadingSql(std::move(logical)));
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

TEST_F(PhysicalPlanTest, ValuesWhichAreNotIntegersFailLowering) {
  CreateTree();
  Exec("UPDATE tree SET self = 'many'");
  auto plan = Plan("FROM tree |> TREE ACCUMULATE UP SUM(self) AS total");
  EXPECT_THAT(plan.status().message(), HasSubstr("'self'"));
}

TEST_F(PhysicalPlanTest, DiagnosticsUseDefiningNamesAfterProjection) {
  CreateTree();
  Exec("UPDATE tree SET self = 'many'");
  PerfettoSqlParser parser(macros_, catalog_,
                           /*pipelines_allowed=*/true);
  parser.Reset(SqlSource::FromExecuteQuery(
      "FROM tree |> TREE ACCUMULATE UP SUM(self) AS total"));
  ASSERT_TRUE(parser.Next()) << parser.status().message();
  auto logical = std::get<PerfettoSqlParser::Pipeline>(parser.statement()).plan;
  // Neither the original input name nor its value is exposed in the result.
  logical.output() = {{"id", logical.output().front().id},
                      {"renamed_total", logical.output().back().id}};
  EXPECT_THAT(LowerReadingSql(std::move(logical)).status().message(),
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

// A relation which only picks and renames a dataframe's columns is read from
// the dataframe, not through SQLite, and is read back so too.
TEST_F(PhysicalPlanTest, ARenamingOfADataframeIsScannedDirectly) {
  CreateDataframeTree();
  auto plan = Compile(
      "FROM (SELECT id, parent_id, self AS own, self AS copy FROM df) "
      "|> TREE ACCUMULATE UP SUM(own) AS total");
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  auto moved = MoveSqlSourcesToDataframeArgs(std::move(*plan));
  EXPECT_TRUE(moved.args.empty());
  auto read = DeserializePlan(SerializePlan(moved.plan), catalog_);
  ASSERT_TRUE(read.ok()) << read.status().message();
  auto lowered = Lower(*read);
  ASSERT_TRUE(lowered.ok()) << lowered.status().message();
  EXPECT_THAT(Names(**lowered),
              ElementsAre("id", "parent_id", "own", "copy", "total"));
  auto rows = Run(**lowered, "copy");
  ASSERT_TRUE(rows.ok()) << rows.status().message();
  EXPECT_THAT(*rows,
              ElementsAre(Pair(0, 10), Pair(1, 20), Pair(2, 30), Pair(3, 40)));
}

}  // namespace
}  // namespace perfetto::trace_processor::pipeline
