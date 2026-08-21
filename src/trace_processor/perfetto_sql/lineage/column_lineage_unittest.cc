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

#include "src/trace_processor/perfetto_sql/lineage/column_lineage.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::lineage {
namespace {

using core::Double;
using core::Int64;
using core::StorageType;
using core::String;

class TestCatalog : public Catalog {
 public:
  void AddDataframe(std::string name, std::vector<ResolvedColumn> columns) {
    dataframes_[std::move(name)] = std::move(columns);
  }
  void AddView(std::string name, std::string sql) {
    views_[std::move(name)] = std::move(sql);
  }

  const std::vector<ResolvedColumn>* Dataframe(
      const std::string& name) const override {
    auto it = dataframes_.find(name);
    return it == dataframes_.end() ? nullptr : &it->second;
  }
  std::optional<std::string> ViewSql(const std::string& name) const override {
    auto it = views_.find(name);
    return it == views_.end() ? std::nullopt : std::make_optional(it->second);
  }

 private:
  std::map<std::string, std::vector<ResolvedColumn>> dataframes_;
  std::map<std::string, std::string> views_;
};

ResolvedColumn Typed(std::string name, StorageType type) {
  ResolvedColumn column;
  column.name = std::move(name);
  column.type = type;
  return column;
}
ResolvedColumn Int(std::string name) {
  return Typed(std::move(name), StorageType{Int64{}});
}
ResolvedColumn Str(std::string name) {
  return Typed(std::move(name), StorageType{String{}});
}

// Renders the result as "name:type" so a test fits on one line.
std::vector<std::string> Show(const std::vector<ResolvedColumn>& columns) {
  std::vector<std::string> out;
  for (const ResolvedColumn& c : columns) {
    std::string type = "?";
    if (c.type) {
      type = c.type->Is<Int64>() ? "int"
                                 : (c.type->Is<Double>() ? "double" : "string");
    }
    out.push_back(c.name + ":" + type);
  }
  return out;
}

class ColumnTypesTest : public ::testing::Test {
 protected:
  ColumnTypesTest() {
    catalog_.AddDataframe("slice", {Int("id"), Int("ts"), Str("name")});
    catalog_.AddDataframe("thread", {Int("utid"), Str("name")});
  }

  std::vector<std::string> Select(const std::string& sql) {
    auto res = ResolveSelect(sql, catalog_);
    EXPECT_TRUE(res.ok()) << sql << ": " << res.status().c_message();
    return res.ok() ? Show(*res) : std::vector<std::string>{};
  }

  TestCatalog catalog_;
};

TEST_F(ColumnTypesTest, ColumnsOfADataframeAreKnown) {
  EXPECT_THAT(Select("SELECT id, name FROM slice"),
              testing::ElementsAre("id:int", "name:string"));
}

TEST_F(ColumnTypesTest, AnExpressionIsUnknown) {
  EXPECT_THAT(Select("SELECT id, ts * 2 AS doubled FROM slice"),
              testing::ElementsAre("id:int", "doubled:?"));
}

TEST_F(ColumnTypesTest, AnAliasIsTheOutputName) {
  EXPECT_THAT(Select("SELECT id AS slice_id FROM slice"),
              testing::ElementsAre("slice_id:int"));
}

TEST_F(ColumnTypesTest, StarIsEveryColumn) {
  EXPECT_THAT(Select("SELECT * FROM slice"),
              testing::ElementsAre("id:int", "ts:int", "name:string"));
}

TEST_F(ColumnTypesTest, AnAliasedTableIsFollowed) {
  EXPECT_THAT(Select("SELECT s.name FROM slice AS s"),
              testing::ElementsAre("name:string"));
}

TEST_F(ColumnTypesTest, ASubqueryIsFollowed) {
  EXPECT_THAT(Select("SELECT inner.id FROM (SELECT id FROM slice) AS inner"),
              testing::ElementsAre("id:int"));
}

TEST_F(ColumnTypesTest, AViewIsFollowedByParsingWhatWasStored) {
  catalog_.AddView("v", "CREATE VIEW v AS SELECT id, ts FROM slice");
  EXPECT_THAT(Select("SELECT ts FROM v"), testing::ElementsAre("ts:int"));
}

TEST_F(ColumnTypesTest, AViewOverAViewIsFollowed) {
  catalog_.AddView("v1", "CREATE VIEW v1 AS SELECT id, name FROM slice");
  catalog_.AddView("v2", "CREATE VIEW v2 AS SELECT name AS n FROM v1");
  EXPECT_THAT(Select("SELECT n FROM v2"), testing::ElementsAre("n:string"));
}

// A view which renames a column still resolves to the column it came from.
TEST_F(ColumnTypesTest, AViewWhichRenamesIsFollowed) {
  catalog_.AddView("v",
                   "CREATE VIEW v AS SELECT id AS a, ts * 2 AS b "
                   "FROM slice");
  EXPECT_THAT(Select("SELECT a, b FROM v"),
              testing::ElementsAre("a:int", "b:?"));
}

TEST_F(ColumnTypesTest, ARelationWhichIsNeitherIsUnknown) {
  EXPECT_THAT(Select("SELECT anything FROM some_sqlite_table"),
              testing::ElementsAre("anything:?"));
}

TEST_F(ColumnTypesTest, AJoinSeesBothSides) {
  EXPECT_THAT(Select("SELECT s.id, t.utid FROM slice AS s, thread AS t"),
              testing::ElementsAre("id:int", "utid:int"));
}

// `name` is in both relations, so it is whatever the two agree on.
TEST_F(ColumnTypesTest, AColumnInBothSidesTakesWhatTheyAgreeOn) {
  EXPECT_THAT(Select("SELECT name FROM slice, thread"),
              testing::ElementsAre("name:string"));
}

TEST_F(ColumnTypesTest, ArmsOfACompoundMustAgree) {
  catalog_.AddDataframe("other", {Str("id")});
  EXPECT_THAT(Select("SELECT id FROM slice UNION ALL SELECT id FROM slice"),
              testing::ElementsAre("id:int"));
  EXPECT_THAT(Select("SELECT id FROM slice UNION ALL SELECT id FROM other"),
              testing::ElementsAre("id:?"));
}

TEST_F(ColumnTypesTest, AnIntegerAndAFloatWeakenToAFloat) {
  catalog_.AddDataframe("f", {Typed("id", StorageType{Double{}})});
  EXPECT_THAT(Select("SELECT id FROM slice UNION ALL SELECT id FROM f"),
              testing::ElementsAre("id:double"));
}

TEST_F(ColumnTypesTest, ABareRelationResolves) {
  auto res = ResolveRelation("slice", catalog_);
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  EXPECT_THAT(Show(*res),
              testing::ElementsAre("id:int", "ts:int", "name:string"));
}

// A view which only renames its source's columns re-exports them, however many
// such views are stacked on top of one another.
TEST_F(ColumnTypesTest, AReexportingViewIsSeenThroughToItsSource) {
  catalog_.AddView("v1", "CREATE VIEW v1 AS SELECT id, name FROM slice");
  catalog_.AddView("v2",
                   "CREATE VIEW v2 AS SELECT id AS a, name AS b "
                   "FROM v1");
  auto res = ResolveRelation("v2", catalog_);
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  EXPECT_EQ(SoleDataframe(*res), "slice");
  EXPECT_EQ((*res)[0].dataframe_column, "id");
  EXPECT_EQ((*res)[1].dataframe_column, "name");
}

TEST_F(ColumnTypesTest, AViewWhichComputesIsNotAReexport) {
  catalog_.AddView("v", "CREATE VIEW v AS SELECT id, ts * 2 AS b FROM slice");
  auto res = ResolveRelation("v", catalog_);
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  EXPECT_EQ(SoleDataframe(*res), "");
  EXPECT_EQ((*res)[0].dataframe, "slice");
  EXPECT_EQ((*res)[1].dataframe, "");
}

TEST_F(ColumnTypesTest, ColumnsFromTwoDataframesHaveNoSoleDataframe) {
  auto res = ResolveSelect("SELECT s.id, t.utid FROM slice AS s, thread AS t",
                           catalog_);
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  EXPECT_EQ(SoleDataframe(*res), "");
  EXPECT_EQ((*res)[0].dataframe, "slice");
  EXPECT_EQ((*res)[1].dataframe, "thread");
}

// A relation which only renames its source's columns can be read as the
// dataframe itself.
TEST_F(ColumnTypesTest, ARenamingViewReadsStraightThrough) {
  catalog_.AddView("v", "CREATE VIEW v AS SELECT id AS a, ts AS b FROM slice");
  EXPECT_EQ(PassthroughDataframe("v", catalog_), "slice");
  EXPECT_EQ(PassthroughDataframe("slice", catalog_), "slice");
}

// The difference from SoleDataframe: every column of a filtered view still
// comes from the dataframe, but the rows are not the dataframe's.
TEST_F(ColumnTypesTest, AFilteredViewDoesNotReadStraightThrough) {
  catalog_.AddView("v", "CREATE VIEW v AS SELECT id FROM slice WHERE ts > 5");
  auto res = ResolveRelation("v", catalog_);
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  EXPECT_EQ(SoleDataframe(*res), "slice");
  EXPECT_EQ(PassthroughDataframe("v", catalog_), "");
}

TEST_F(ColumnTypesTest, AnythingWhichTouchesTheRowsDoesNotReadThrough) {
  catalog_.AddView("computed",
                   "CREATE VIEW computed AS SELECT ts * 2 AS a FROM slice");
  catalog_.AddView("limited",
                   "CREATE VIEW limited AS SELECT id FROM slice LIMIT 10");
  catalog_.AddView("distinct_",
                   "CREATE VIEW distinct_ AS SELECT DISTINCT id FROM slice");
  catalog_.AddView("joined",
                   "CREATE VIEW joined AS SELECT s.id FROM slice AS s, "
                   "thread AS t");
  EXPECT_EQ(PassthroughDataframe("computed", catalog_), "");
  EXPECT_EQ(PassthroughDataframe("limited", catalog_), "");
  EXPECT_EQ(PassthroughDataframe("distinct_", catalog_), "");
  EXPECT_EQ(PassthroughDataframe("joined", catalog_), "");
}

// Traced through any number of renaming views.
TEST_F(ColumnTypesTest, ReadingThroughSurvivesAChainOfViews) {
  catalog_.AddView("v1", "CREATE VIEW v1 AS SELECT id, ts FROM slice");
  catalog_.AddView("v2", "CREATE VIEW v2 AS SELECT id AS a, ts AS b FROM v1");
  catalog_.AddView("v3", "CREATE VIEW v3 AS SELECT a, b FROM v2");
  EXPECT_EQ(PassthroughDataframe("v3", catalog_), "slice");
}

// But not through a filter buried in the middle of the chain.
TEST_F(ColumnTypesTest, AFilterAnywhereInTheChainStopsIt) {
  catalog_.AddView("v1", "CREATE VIEW v1 AS SELECT id FROM slice WHERE ts > 5");
  catalog_.AddView("v2", "CREATE VIEW v2 AS SELECT id AS a FROM v1");
  EXPECT_EQ(PassthroughDataframe("v2", catalog_), "");
}

}  // namespace
}  // namespace perfetto::trace_processor::lineage
