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

#include "src/trace_processor/perfetto_sql/lineage/connection_catalog.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/perfetto_sql/lineage/column_lineage.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::lineage {
namespace {

class ConnectionCatalogTest : public ::testing::Test {
 protected:
  void Exec(const std::string& sql) {
    auto res = connection_->Execute(SqlSource::FromExecuteQuery(sql));
    ASSERT_TRUE(res.ok()) << sql << ": " << res.status().c_message();
  }

  StringPool pool_;
  std::unique_ptr<PerfettoSqlConnection> connection_ =
      PerfettoSqlConnection::CreateConnectionToNewDatabase(&pool_, true);
  ConnectionCatalog catalog_{connection_.get()};
};

TEST_F(ConnectionCatalogTest, APerfettoTableIsADataframe) {
  Exec("CREATE PERFETTO TABLE t AS SELECT 1 AS a, 'x' AS b");
  const std::vector<ResolvedColumn>* columns = catalog_.Dataframe("t");
  ASSERT_NE(columns, nullptr);
  std::vector<std::string> names;
  for (const ResolvedColumn& column : *columns) {
    EXPECT_TRUE(column.type.has_value()) << column.name;
    names.push_back(column.name);
  }
  // A perfetto table carries an id column of its own alongside the selected
  // columns.
  EXPECT_THAT(names, testing::IsSupersetOf({"a", "b"}));
}

TEST_F(ConnectionCatalogTest, APlainSqliteTableIsNot) {
  Exec("CREATE TABLE t(a INTEGER)");
  EXPECT_EQ(catalog_.Dataframe("t"), nullptr);
  EXPECT_FALSE(catalog_.ViewSql("t").has_value());
}

TEST_F(ConnectionCatalogTest, AViewHandsBackWhatSqliteStored) {
  Exec("CREATE PERFETTO TABLE t AS SELECT 1 AS a");
  Exec("CREATE VIEW v AS SELECT a FROM t");
  std::optional<std::string> sql = catalog_.ViewSql("v");
  ASSERT_TRUE(sql.has_value());
  EXPECT_THAT(*sql, testing::HasSubstr("SELECT a FROM t"));
}

TEST_F(ConnectionCatalogTest, SomethingWhichIsNeitherIsNeither) {
  EXPECT_EQ(catalog_.Dataframe("nope"), nullptr);
  EXPECT_FALSE(catalog_.ViewSql("nope").has_value());
}

// The point of the catalog: a query over a real table comes back typed.
TEST_F(ConnectionCatalogTest, AQueryOverADataframeResolves) {
  Exec("CREATE PERFETTO TABLE t AS SELECT 1 AS id, 2.5 AS weight");
  Exec("CREATE VIEW v AS SELECT id AS renamed, weight FROM t");

  auto res =
      ResolveSelect("SELECT renamed, weight * 2 AS scaled FROM v", catalog_);
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  ASSERT_EQ(res->size(), 2u);
  EXPECT_TRUE((*res)[0].type.has_value());
  EXPECT_EQ((*res)[0].dataframe, "t");
  EXPECT_EQ((*res)[0].dataframe_column, "id");
  EXPECT_FALSE((*res)[1].type.has_value());
}

TEST_F(ConnectionCatalogTest, AViewWhichOnlyRenamesReexportsItsSource) {
  Exec("CREATE PERFETTO TABLE t AS SELECT 1 AS id, 2 AS other");
  Exec("CREATE VIEW v AS SELECT id, other FROM t");
  Exec("CREATE VIEW w AS SELECT id AS a, other AS b FROM v");

  auto res = ResolveRelation("w", catalog_);
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  EXPECT_EQ(SoleDataframe(*res), "t");
}

}  // namespace
}  // namespace perfetto::trace_processor::lineage
