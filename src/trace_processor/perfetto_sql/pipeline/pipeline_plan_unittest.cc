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

#include "src/trace_processor/perfetto_sql/pipeline/pipeline_plan.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "src/perfetto_sql/analysis/relation.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/perfetto_sql/pipeline/pipeline_syntax.h"
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

class NoCatalog : public perfetto_sql::analysis::Catalog {
 public:
  std::optional<perfetto_sql::analysis::LeafRelation> FindLeafRelation(
      std::string_view) const override {
    return std::nullopt;
  }
  std::optional<std::string> FindViewSql(std::string_view) const override {
    return std::nullopt;
  }
};

// An integer cell, whether the column is flat or carries a type per row.
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

class PipelinePlanTest : public ::testing::Test {
 protected:
  PipelinePlanTest()
      : connection_(SqliteConnection::CreateConnectionToNewDatabase()) {
    env_.connection = connection_.get();
    env_.pool = &pool_;
    env_.catalog = &catalog_;
    env_.find_dataframe = [this](std::string_view name)
        -> std::shared_ptr<const dataframe::Dataframe> {
      auto it = dataframes_.find(std::string(name));
      return it == dataframes_.end() ? nullptr : it->second;
    };
  }

  void Exec(const std::string& sql) {
    auto statement =
        connection_->PrepareStatement(SqlSource::FromExecuteQuery(sql));
    while (statement.Step()) {
    }
    ASSERT_TRUE(statement.status().ok()) << statement.status().c_message();
  }

  // A tree of four nodes, written parent first:
  //   0 (10) -> 1 (20) -> 3 (40)
  //          -> 2 (30)
  void CreateTree() {
    Exec("CREATE TABLE tree(id INTEGER, parent_id INTEGER, self INTEGER)");
    Exec(
        "INSERT INTO tree VALUES (0, NULL, 10), (1, 0, 20), (2, 0, 30), "
        "(3, 1, 40)");
  }

  base::StatusOr<std::unique_ptr<PipelinePlan>> Plan(const std::string& sql) {
    auto syntax = ParsePipeline(SqlSource::FromExecuteQuery(sql));
    if (!syntax.ok()) {
      return syntax.status();
    }
    return PlanPipeline(*syntax, env_);
  }

  std::vector<std::string> Names(const PipelinePlan& plan) {
    std::vector<std::string> names;
    for (const PipelinePlan::Column& column : plan.columns()) {
      names.push_back(column.name);
    }
    return names;
  }

  // Runs `plan` and returns the value of `value` for each `id`.
  base::StatusOr<std::map<int64_t, std::optional<int64_t>>> Run(
      const PipelinePlan& plan,
      const std::string& value) {
    uint32_t id_column = 0;
    uint32_t value_column = 0;
    for (const PipelinePlan::Column& column : plan.columns()) {
      if (column.name == "id") {
        id_column = column.index;
      } else if (column.name == value) {
        value_column = column.index;
      }
    }
    std::map<int64_t, std::optional<int64_t>> out;
    RowCursor cursor(plan.source());
    for (bool row = cursor.Open(); row; row = cursor.Next()) {
      out[*IntAt(cursor, id_column)] = IntAt(cursor, value_column);
    }
    if (!cursor.status().ok()) {
      return cursor.status();
    }
    return out;
  }

  StringPool pool_;
  std::unique_ptr<SqliteConnection> connection_;
  NoCatalog catalog_;
  std::map<std::string, std::shared_ptr<dataframe::Dataframe>> dataframes_;
  PlanEnvironment env_;
};

TEST_F(PipelinePlanTest, AFromAloneReadsTheSource) {
  CreateTree();
  auto plan = Plan("FROM tree");
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  EXPECT_THAT(Names(**plan), ElementsAre("id", "parent_id", "self"));
  auto rows = Run(**plan, "self");
  ASSERT_TRUE(rows.ok()) << rows.status().message();
  EXPECT_THAT(*rows,
              ElementsAre(Pair(0, 10), Pair(1, 20), Pair(2, 30), Pair(3, 40)));
}

TEST_F(PipelinePlanTest, AccumulateUpSumsEachSubtree) {
  CreateTree();
  auto plan = Plan("FROM tree |> TREE ACCUMULATE UP SUM(self) AS total");
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  EXPECT_THAT(Names(**plan), ElementsAre("id", "parent_id", "self", "total"));
  auto rows = Run(**plan, "total");
  ASSERT_TRUE(rows.ok()) << rows.status().message();
  EXPECT_THAT(*rows,
              ElementsAre(Pair(0, 100), Pair(1, 60), Pair(2, 30), Pair(3, 40)));
}

TEST_F(PipelinePlanTest, AccumulateDownSumsEachRootPath) {
  CreateTree();
  auto plan = Plan("FROM tree |> TREE ACCUMULATE DOWN SUM(self) AS path");
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  auto rows = Run(**plan, "path");
  ASSERT_TRUE(rows.ok()) << rows.status().message();
  EXPECT_THAT(*rows,
              ElementsAre(Pair(0, 10), Pair(1, 30), Pair(2, 40), Pair(3, 70)));
}

// The FROM clause is any SQL a FROM clause accepts.
TEST_F(PipelinePlanTest, TheSourceCanBeArbitrarySql) {
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

TEST_F(PipelinePlanTest, StagesAndAggregatesCompose) {
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

// A name which is a dataframe is read without SQLite, which here has no such
// table at all.
TEST_F(PipelinePlanTest, ADataframeIsScannedDirectly) {
  dataframe::AdhocDataframeBuilder builder({"id", "parent_id", "self"}, &pool_);
  const std::vector<std::pair<int64_t, std::optional<int64_t>>> tree = {
      {3, 1}, {1, 0}, {2, 0}, {0, std::nullopt}};
  for (const auto& [id, parent] : tree) {
    ASSERT_TRUE(builder.PushNonNull(0, id));
    if (parent) {
      ASSERT_TRUE(builder.PushNonNull(1, *parent));
    } else {
      builder.PushNull(1);
    }
    ASSERT_TRUE(builder.PushNonNull(2, (id + 1) * 10));
  }
  auto df = std::move(builder).Build();
  ASSERT_TRUE(df.ok()) << df.status().message();
  dataframes_.emplace("df",
                      std::make_shared<dataframe::Dataframe>(std::move(*df)));

  auto plan = Plan("FROM df |> TREE ACCUMULATE UP SUM(self) AS total");
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  EXPECT_THAT(Names(**plan), ElementsAre("id", "parent_id", "self", "total"));
  auto rows = Run(**plan, "total");
  ASSERT_TRUE(rows.ok()) << rows.status().message();
  EXPECT_THAT(*rows,
              ElementsAre(Pair(0, 100), Pair(1, 60), Pair(2, 30), Pair(3, 40)));
}

TEST_F(PipelinePlanTest, PlanningErrors) {
  CreateTree();
  EXPECT_THAT(Plan("FROM nope").status().message(),
              HasSubstr("no such table: nope"));
  EXPECT_THAT(Plan("FROM (SELECT id, self FROM tree) |> TREE ACCUMULATE UP "
                   "SUM(self) AS total")
                  .status()
                  .message(),
              HasSubstr("no such column: 'parent_id'"));
  EXPECT_THAT(Plan("FROM tree |> TREE ACCUMULATE UP SUM(nope) AS total")
                  .status()
                  .message(),
              HasSubstr("no such column: 'nope'"));
  EXPECT_THAT(Plan("FROM tree |> TREE ACCUMULATE UP MAX(self) AS total")
                  .status()
                  .message(),
              HasSubstr("only SUM(column) is supported so far, not MAX"));
  EXPECT_THAT(Plan("FROM tree |> TREE ACCUMULATE UP SUM(self) AS SELF")
                  .status()
                  .message(),
              HasSubstr("a column named 'SELF' already exists"));
  EXPECT_THAT(
      Plan("FROM tree |> TREE ACCUMULATE UP SUM(self) AS a, SUM(self) AS a")
          .status()
          .message(),
      HasSubstr("'a' is named twice"));
  EXPECT_THAT(Plan("FROM tree a JOIN tree b ON a.id = b.parent_id |> "
                   "TREE ACCUMULATE UP SUM(self) AS total")
                  .status()
                  .message(),
              HasSubstr("column 'self' is ambiguous"));
}

TEST_F(PipelinePlanTest, ValuesWhichAreNotIntegersFailTheRun) {
  CreateTree();
  Exec("INSERT INTO tree VALUES (4, 0, 'many')");
  auto plan = Plan("FROM tree |> TREE ACCUMULATE UP SUM(self) AS total");
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  EXPECT_THAT(Run(**plan, "total").status().message(), HasSubstr("'self'"));
}

TEST_F(PipelinePlanTest, AnIncompleteTreeFailsTheRun) {
  CreateTree();
  auto plan = Plan(
      "FROM (SELECT * FROM tree WHERE id != 1) "
      "|> TREE ACCUMULATE UP SUM(self) AS total");
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  EXPECT_FALSE(Run(**plan, "total").ok());
}

TEST_F(PipelinePlanTest, APlanCanRunMoreThanOnce) {
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
