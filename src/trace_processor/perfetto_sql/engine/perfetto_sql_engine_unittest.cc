/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"

#include "src/trace_processor/sqlite/sql_source.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

class PerfettoSqlEngineTest : public ::testing::Test {
 protected:
  StringPool pool_;
  PerfettoSqlEngine engine_{&pool_};
};

sql_modules::RegisteredModule CreateTestModule(
    std::vector<std::pair<std::string, std::string>> files) {
  sql_modules::RegisteredModule result;
  for (auto& file : files) {
    result.include_key_to_file[file.first] =
        sql_modules::RegisteredModule::ModuleFile{file.second, false};
  }
  return result;
}

TEST_F(PerfettoSqlEngineTest, CreatePerfettoFunctionSmoke) {
  auto res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO FUNCTION foo() RETURNS INT AS select 1"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();

  res = engine_.Execute(
      SqlSource::FromExecuteQuery("creatE PeRfEttO FUNCTION foo(x INT, y LONG) "
                                  "RETURNS INT AS select :x + :y"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlEngineTest, CreatePerfettoFunctionArgs) {
  auto res = engine_.ExecuteUntilLastStatement(
      SqlSource::FromExecuteQuery("creatE PeRfEttO FUNCTION foo(x INT, y LONG) "
                                  "RETURNS INT AS select $x + $y;"
                                  "SELECT foo(1, 2)"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  ASSERT_FALSE(res->stmt.IsDone());
  ASSERT_EQ(sqlite3_column_int64(res->stmt.sqlite_stmt(), 0), 3);
  ASSERT_FALSE(res->stmt.Step());
}

TEST_F(PerfettoSqlEngineTest, CreatePerfettoFunctionError) {
  auto res = engine_.ExecuteUntilLastStatement(
      SqlSource::FromExecuteQuery("creatE PeRfEttO FUNCTION foo(x INT, y LONG) "
                                  "AS select $x + $y;"
                                  "SELECT foo(1, 2)"));
  ASSERT_FALSE(res.ok());
}

TEST_F(PerfettoSqlEngineTest, CreatePerfettoTableSmoke) {
  auto res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo AS SELECT 42 AS bar"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlEngineTest, CreatePerfettoTableStringSmoke) {
  auto res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo AS SELECT 'foo' AS bar"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlEngineTest, CreatePerfettoTableWithSchemaSmoke) {
  auto res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo(bar INT) AS SELECT 42 AS bar"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();

  res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo2(bar INT) AS SELECT 42 AS bar; SELECT 1"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlEngineTest, CreatePerfettoTableWithIncorrectColumns) {
  auto res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo(x INT) AS SELECT 1 as y"));
  ASSERT_FALSE(res.ok());
  EXPECT_THAT(
      res.status().c_message(),
      testing::EndsWith("CREATE PERFETTO TABLE: the following columns are "
                        "declared in the schema, but do not exist: x; and the "
                        "folowing columns exist, but are not declared: y"));
}

TEST_F(PerfettoSqlEngineTest, CreatePerfettoTableDrop) {
  auto res_create = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo AS SELECT 'foo' AS bar"));
  ASSERT_TRUE(res_create.ok());

  auto res_drop =
      engine_.Execute(SqlSource::FromExecuteQuery("DROP TABLE foo"));
  ASSERT_TRUE(res_drop.ok());
}

TEST_F(PerfettoSqlEngineTest, CreatePerfettoTableValues) {
  auto res = engine_.ExecuteUntilLastStatement(
      SqlSource::FromExecuteQuery("creatE PeRfEttO TABLE foo AS "
                                  "SELECT 42 as bar;"
                                  "SELECT * from foo"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  ASSERT_FALSE(res->stmt.IsDone());
  ASSERT_EQ(sqlite3_column_int64(res->stmt.sqlite_stmt(), 0), 42);
  ASSERT_FALSE(res->stmt.Step());
}

TEST_F(PerfettoSqlEngineTest, CreateTableFunctionDupe) {
  auto res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO FUNCTION foo() RETURNS TABLE(x INT) AS "
      "select 1 AS x"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();

  res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO FUNCTION foo() RETURNS TABLE(x INT) AS "
      "select 1 AS x"));
  ASSERT_FALSE(res.ok());

  res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE OR REPLACE PERFETTO FUNCTION foo() RETURNS TABLE(x INT) AS "
      "select 2 AS x"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlEngineTest, CreatePerfettoViewSmoke) {
  auto res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO VIEW foo AS SELECT 42 AS bar"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlEngineTest, CreatePerfettoViewWithSchemaSmoke) {
  auto res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO VIEW foo(bar INT) AS SELECT 42 AS bar"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();

  res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO VIEW foo2(bar INT) AS SELECT 42 AS bar; SELECT 1"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlEngineTest, CreatePerfettoViewWithIncorrectColumns) {
  auto res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO VIEW foo(x INT) AS SELECT 1 as y"));
  ASSERT_FALSE(res.ok());
  EXPECT_THAT(
      res.status().c_message(),
      testing::EndsWith("CREATE PERFETTO VIEW: the following columns are "
                        "declared in the schema, but do not exist: x; and the "
                        "folowing columns exist, but are not declared: y"));
}

TEST_F(PerfettoSqlEngineTest, CreateMacro) {
  auto res_create = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO MACRO foo() RETURNS TableOrSubquery AS select 42 AS x"));
  ASSERT_TRUE(res_create.ok()) << res_create.status().c_message();

  res_create = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO MACRO bar(x TableOrSubquery) RETURNS TableOrSubquery AS "
      "select * from $x"));
  ASSERT_TRUE(res_create.ok()) << res_create.status().c_message();

  auto res = engine_.ExecuteUntilLastStatement(
      SqlSource::FromExecuteQuery("bar!((foo!()))"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  ASSERT_FALSE(res->stmt.IsDone());
  ASSERT_EQ(sqlite3_column_int64(res->stmt.sqlite_stmt(), 0), 42);
  ASSERT_FALSE(res->stmt.Step());
}

TEST_F(PerfettoSqlEngineTest, IncludeAll) {
  engine_.RegisterModule(
      "foo", CreateTestModule(
                 {{"foo.foo", "CREATE PERFETTO TABLE foo AS SELECT 42 AS x"}}));
  engine_.RegisterModule(
      "bar",
      CreateTestModule(
          {{"bar.bar", "CREATE PERFETTO TABLE bar AS SELECT 42 AS x "}}));

  auto res_create =
      engine_.Execute(SqlSource::FromExecuteQuery("INCLUDE PERFETTO MODULE *"));
  ASSERT_TRUE(res_create.ok()) << res_create.status().c_message();
  ASSERT_TRUE(
      engine_.FindModule("foo")->include_key_to_file["foo.foo"].included);
  ASSERT_TRUE(
      engine_.FindModule("bar")->include_key_to_file["bar.bar"].included);
}

TEST_F(PerfettoSqlEngineTest, IncludeModule) {
  engine_.RegisterModule(
      "foo", CreateTestModule({
                 {"foo.foo1", "CREATE PERFETTO TABLE foo1 AS SELECT 42 AS x"},
                 {"foo.foo2", "CREATE PERFETTO TABLE foo2 AS SELECT 42 AS x"},
             }));
  engine_.RegisterModule(
      "bar",
      CreateTestModule(
          {{"bar.bar", "CREATE PERFETTO TABLE bar AS SELECT 42 AS x "}}));

  auto res_create = engine_.Execute(
      SqlSource::FromExecuteQuery("INCLUDE PERFETTO MODULE foo.*"));
  ASSERT_TRUE(res_create.ok()) << res_create.status().c_message();
  ASSERT_TRUE(
      engine_.FindModule("foo")->include_key_to_file["foo.foo1"].included);
  ASSERT_TRUE(
      engine_.FindModule("foo")->include_key_to_file["foo.foo2"].included);
  ASSERT_FALSE(
      engine_.FindModule("bar")->include_key_to_file["bar.bar"].included);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
