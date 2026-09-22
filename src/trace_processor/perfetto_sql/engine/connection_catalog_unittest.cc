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

#include "src/trace_processor/perfetto_sql/engine/connection_catalog.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "src/perfetto_sql/analysis/relation.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/perfetto_sql/schema/type_mapping.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

namespace analysis = ::perfetto::perfetto_sql::analysis;
using sql_schema::ToStorageType;
using ::testing::ElementsAre;
using ::testing::Optional;

struct ParserDeleter {
  void operator()(SyntaqliteParser* parser) const {
    syntaqlite_parser_destroy(parser);
  }
};
using ScopedParser = std::unique_ptr<SyntaqliteParser, ParserDeleter>;

class ConnectionCatalogTest : public ::testing::Test {
 protected:
  void Exec(const std::string& sql) {
    auto result = connection_->Execute(SqlSource::FromExecuteQuery(sql));
    ASSERT_TRUE(result.ok()) << sql << ": " << result.status().c_message();
  }

  base::StatusOr<std::vector<std::optional<core::StorageType>>> Types(
      const std::string& sql) {
    ScopedParser parser(syntaqlite_parser_create_perfetto(nullptr));
    syntaqlite_parser_reset(parser.get(), sql.data(),
                            static_cast<uint32_t>(sql.size()));
    if (syntaqlite_parser_next(parser.get()) != SYNTAQLITE_PARSE_OK) {
      return base::ErrStatus("could not parse test query");
    }

    analysis::RelationAnalyzer analyzer(catalog_);
    ASSIGN_OR_RETURN(analysis::RelationLineage lineage,
                     analyzer.AnalyzeQuery(
                         {parser.get(), syntaqlite_result_root(parser.get())}));
    std::vector<std::optional<core::StorageType>> types;
    types.reserve(lineage.columns().size());
    for (const analysis::ColumnLineage& column : lineage.columns()) {
      std::optional<analysis::ColumnType> type = column.type();
      types.push_back(type ? std::make_optional(ToStorageType(*type))
                           : std::nullopt);
    }
    return types;
  }

  StringPool pool_;
  std::unique_ptr<PerfettoSqlConnection> connection_ =
      PerfettoSqlConnection::CreateConnectionToNewDatabase(&pool_, true);
  ConnectionCatalog catalog_{connection_.get()};
};

TEST_F(ConnectionCatalogTest, ReadsDataframeStorageTypes) {
  Exec(
      "CREATE PERFETTO TABLE t AS "
      "SELECT 1 AS id, 2.5 AS weight, 'x' AS name");

  auto types = Types("SELECT id, weight, name FROM t");
  ASSERT_TRUE(types.ok()) << types.status().c_message();
  ASSERT_EQ(types->size(), 3u);
  ASSERT_TRUE((*types)[0].has_value());
  ASSERT_TRUE((*types)[1].has_value());
  ASSERT_TRUE((*types)[2].has_value());
  EXPECT_TRUE((*types)[0]->Is<core::Uint32>());
  EXPECT_TRUE((*types)[1]->Is<core::Double>());
  EXPECT_TRUE((*types)[2]->Is<core::String>());
}

TEST_F(ConnectionCatalogTest, ReadsReplacedDataframeSchema) {
  Exec("CREATE PERFETTO TABLE t AS SELECT 1 AS value");
  ASSERT_TRUE(Types("SELECT value FROM t").ok());
  Exec("CREATE OR REPLACE PERFETTO TABLE t AS SELECT 'x' AS value");

  auto types = Types("SELECT value FROM t");
  ASSERT_TRUE(types.ok()) << types.status().c_message();
  ASSERT_EQ(types->size(), 1u);
  ASSERT_TRUE((*types)[0].has_value());
  EXPECT_TRUE((*types)[0]->Is<core::String>());
}

TEST_F(ConnectionCatalogTest, UsesTemporaryViewDefinition) {
  Exec("CREATE PERFETTO TABLE ints AS SELECT 1 AS value");
  Exec("CREATE PERFETTO TABLE strings AS SELECT 'x' AS value");
  Exec("CREATE VIEW v AS SELECT value FROM ints");
  Exec("CREATE TEMP VIEW v AS SELECT value FROM strings");

  auto types = Types("SELECT value FROM v");
  ASSERT_TRUE(types.ok()) << types.status().c_message();
  ASSERT_EQ(types->size(), 1u);
  ASSERT_TRUE((*types)[0].has_value());
  EXPECT_TRUE((*types)[0]->Is<core::String>());
}

TEST_F(ConnectionCatalogTest, FindsViewsCreatedAfterAnEarlierLookup) {
  Exec("CREATE PERFETTO TABLE ints AS SELECT 1 AS value");
  Exec("CREATE PERFETTO TABLE strings AS SELECT 'x' AS value");
  EXPECT_EQ(catalog_.FindViewSql("v"), std::nullopt);

  Exec("CREATE VIEW v AS SELECT value FROM ints");
  auto types = Types("SELECT value FROM v");
  ASSERT_TRUE(types.ok()) << types.status().c_message();
  ASSERT_TRUE((*types)[0].has_value());
  EXPECT_TRUE((*types)[0]->Is<core::Uint32>());

  Exec("DROP VIEW v");
  Exec("CREATE VIEW v AS SELECT value FROM strings");
  types = Types("SELECT value FROM v");
  ASSERT_TRUE(types.ok()) << types.status().c_message();
  ASSERT_TRUE((*types)[0].has_value());
  EXPECT_TRUE((*types)[0]->Is<core::String>());

  Exec("DROP VIEW v");
  EXPECT_EQ(catalog_.FindViewSql("v"), std::nullopt);
  EXPECT_EQ(catalog_.FindViewSql("it's"), std::nullopt);
}

TEST_F(ConnectionCatalogTest, RequiresEveryOriginToHaveTheSameType) {
  Exec("CREATE PERFETTO TABLE ints AS SELECT 1 AS value");
  Exec("CREATE PERFETTO TABLE more_ints AS SELECT 2 AS value");
  Exec("CREATE PERFETTO TABLE strings AS SELECT 'x' AS value");

  auto same = Types("SELECT value FROM ints JOIN more_ints USING(value)");
  ASSERT_TRUE(same.ok()) << same.status().c_message();
  ASSERT_EQ(same->size(), 1u);
  ASSERT_TRUE((*same)[0].has_value());
  EXPECT_TRUE((*same)[0]->Is<core::Uint32>());

  auto different = Types("SELECT value FROM ints JOIN strings USING(value)");
  ASSERT_TRUE(different.ok()) << different.status().c_message();
  ASSERT_EQ(different->size(), 1u);
  EXPECT_FALSE((*different)[0].has_value());
}

TEST_F(ConnectionCatalogTest, ResolvesTableNamesCaseInsensitively) {
  Exec("CREATE PERFETTO TABLE t AS SELECT 1 AS id");

  auto types = Types("SELECT id FROM T");
  ASSERT_TRUE(types.ok()) << types.status().c_message();
  ASSERT_EQ(types->size(), 1u);
  ASSERT_TRUE((*types)[0].has_value());
  EXPECT_TRUE((*types)[0]->Is<core::Uint32>());
}

TEST_F(ConnectionCatalogTest, ResolvesViewNamesCaseInsensitively) {
  Exec("CREATE PERFETTO TABLE t AS SELECT 1 AS id");
  Exec("CREATE VIEW v AS SELECT id FROM t");

  auto types = Types("SELECT id FROM V");
  ASSERT_TRUE(types.ok()) << types.status().c_message();
  ASSERT_EQ(types->size(), 1u);
  ASSERT_TRUE((*types)[0].has_value());
  EXPECT_TRUE((*types)[0]->Is<core::Uint32>());
}

// The dataframe column behind each result column of `sql`, as `name=column`,
// or nothing if the query cannot be read straight out of one dataframe.
std::optional<std::vector<std::string>> Resolved(
    const ConnectionCatalog& catalog,
    const std::string& sql) {
  auto described = catalog.DescribeQuery(
      SqlSource::FromTraceProcessorImplementation(sql), {});
  if (!described.ok() || !described->dataframe) {
    return std::nullopt;
  }
  const auto& found = described->dataframe;
  std::vector<std::string> out{found->name};
  for (const auto& column : found->columns) {
    out.push_back(column.name + "=" +
                  found->dataframe->column_names()[column.index]);
  }
  return out;
}

TEST_F(ConnectionCatalogTest, FollowsViewsDownToTheDataframe) {
  Exec("CREATE PERFETTO TABLE t AS SELECT 1 AS id, 2 AS ts, 'x' AS name");
  Exec("CREATE PERFETTO VIEW one AS SELECT id, ts, name AS label FROM t");
  Exec("CREATE PERFETTO VIEW two AS SELECT label, id AS slice_id, ts FROM one");
  Exec("CREATE PERFETTO VIEW three AS SELECT *, slice_id AS again FROM two");

  EXPECT_THAT(Resolved(catalog_, "SELECT * FROM three"),
              Optional(ElementsAre("t", "label=name", "slice_id=id", "ts=ts",
                                   "again=id")));
  EXPECT_THAT(
      Resolved(catalog_, "SELECT ts AS t0 FROM (SELECT * FROM two) AS q"),
      Optional(ElementsAre("t", "t0=ts")));
}

TEST_F(ConnectionCatalogTest, OnlyFollowsViewsWhichKeepEveryRowAndValue) {
  Exec("CREATE PERFETTO TABLE t AS SELECT 1 AS id, 2 AS ts");
  Exec("CREATE PERFETTO TABLE u AS SELECT 1 AS id, 3 AS dur");
  Exec("CREATE PERFETTO VIEW filtered AS SELECT * FROM t WHERE ts > 0");
  Exec("CREATE PERFETTO VIEW ordered AS SELECT * FROM t ORDER BY ts");
  Exec("CREATE PERFETTO VIEW computed AS SELECT id, ts + 1 AS ts FROM t");
  Exec(
      "CREATE PERFETTO VIEW joined AS SELECT t.id, u.dur FROM t JOIN u USING "
      "(id)");
  // A view which is fine, over one which is not.
  Exec("CREATE PERFETTO VIEW over_filtered AS SELECT id FROM filtered");

  for (const char* view :
       {"filtered", "ordered", "computed", "joined", "over_filtered"}) {
    EXPECT_EQ(Resolved(catalog_, std::string("SELECT * FROM ") + view),
              std::nullopt)
        << view;
  }
  EXPECT_EQ(Resolved(catalog_, "SELECT * FROM t LIMIT 1"), std::nullopt);
  EXPECT_EQ(Resolved(catalog_, "SELECT DISTINCT id FROM t"), std::nullopt);
  EXPECT_EQ(Resolved(catalog_, "SELECT * FROM no_such_table"), std::nullopt);
}

}  // namespace
}  // namespace perfetto::trace_processor
