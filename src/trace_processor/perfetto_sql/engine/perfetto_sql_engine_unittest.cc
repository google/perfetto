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
#include "src/trace_processor/tables/slice_tables_py.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

class PerfettoSqlEngineTest : public ::testing::Test {
 protected:
  StringPool pool_;
  PerfettoSqlEngine engine_{&pool_, true};
};

sql_modules::RegisteredPackage CreateTestPackage(
    std::vector<std::pair<std::string, std::string>> files) {
  sql_modules::RegisteredPackage result;
  for (auto& file : files) {
    result.modules[file.first] =
        sql_modules::RegisteredPackage::ModuleFile{file.second, false};
  }
  return result;
}

// These are the smoke tests for the perfetto SQL engine, focusing on
// ensuring that the correct statements do not return an error and that
// incorrect statements do.
//
// Functional tests are covered by the diff tests in
// test/trace_processor/diff_tests/syntax/perfetto_sql.

TEST_F(PerfettoSqlEngineTest, Function_Create) {
  auto res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO FUNCTION foo() RETURNS INT AS select 1"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();

  res = engine_.Execute(
      SqlSource::FromExecuteQuery("creatE PeRfEttO FUNCTION foo(x INT, y LONG) "
                                  "RETURNS INT AS select $x + $y"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlEngineTest, Function_CreateWithArgs) {
  auto res = engine_.ExecuteUntilLastStatement(
      SqlSource::FromExecuteQuery("creatE PeRfEttO FUNCTION foo(x INT, y LONG) "
                                  "RETURNS INT AS select $x + $y;"
                                  "SELECT foo(1, 2)"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  ASSERT_FALSE(res->stmt.IsDone());
  ASSERT_EQ(sqlite3_column_int64(res->stmt.sqlite_stmt(), 0), 3);
  ASSERT_FALSE(res->stmt.Step());
}

TEST_F(PerfettoSqlEngineTest, Function_Invalid) {
  auto res = engine_.ExecuteUntilLastStatement(
      SqlSource::FromExecuteQuery("creatE PeRfEttO FUNCTION foo(x INT, y LONG) "
                                  "AS select $x + $y;"
                                  "SELECT foo(1, 2)"));
  ASSERT_FALSE(res.ok());
}

TEST_F(PerfettoSqlEngineTest, Function_Duplicates) {
  auto res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO FUNCTION foo() RETURNS INT AS SELECT 1"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();

  res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO FUNCTION foo() RETURNS INT AS SELECT 2"));
  ASSERT_FALSE(res.ok());

  res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE OR REPLACE PERFETTO FUNCTION foo() RETURNS INT AS SELECT 3"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlEngineTest, TableFunction_Create) {
  auto res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO FUNCTION foo() RETURNS TABLE(x INT) AS "
      "select 1 AS x"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlEngineTest, TableFunction_Duplicates) {
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

TEST_F(PerfettoSqlEngineTest, Table_Create) {
  auto res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo AS SELECT 42 AS bar"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlEngineTest, Table_StringColumns) {
  auto res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo AS SELECT 'foo' AS bar"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlEngineTest, Table_Schema) {
  auto res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo(bar INT) AS SELECT 42 AS bar"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();

  res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo2(bar INT) AS SELECT 42 AS bar; SELECT 1"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlEngineTest, Table_Schema_EmptyTable) {
  // This test checks that the type checks correctly work on empty tables (and
  // that columns with no data do not default to "int").
  auto res = engine_.Execute(
      SqlSource::FromExecuteQuery("CREATE PERFETTO TABLE foo(bar STRING) AS "
                                  "SELECT 'bar' as bar WHERE bar = 'foo'"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlEngineTest, Table_Schema_NullColumn) {
  // This test checks that the type checks correctly work on columns without
  // data (and that columns with no non-NULL data do not default to "int").
  auto res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo(bar STRING) AS SELECT NULL as bar"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlEngineTest, Table_IncorrectSchema_MissingColumn) {
  auto res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo(x INT) AS SELECT 1 as y"));
  ASSERT_FALSE(res.ok());
  EXPECT_THAT(
      res.status().c_message(),
      testing::EndsWith("CREATE PERFETTO TABLE: the following columns are "
                        "declared in the schema, but do not exist: x; and the "
                        "folowing columns exist, but are not declared: y"));
}

TEST_F(PerfettoSqlEngineTest, Table_IncorrectSchema_IncorrectType) {
  auto res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo(x INT) AS SELECT '1' as x"));
  ASSERT_FALSE(res.ok());
  EXPECT_THAT(
      res.status().c_message(),
      testing::EndsWith("CREATE PERFETTO TABLE(foo): column 'x' declared as "
                        "INT (LONG) in the schema, but STRING found"));
}

TEST_F(PerfettoSqlEngineTest, Table_Drop) {
  auto res_create = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo AS SELECT 'foo' AS bar"));
  ASSERT_TRUE(res_create.ok());

  auto res_drop =
      engine_.Execute(SqlSource::FromExecuteQuery("DROP TABLE foo"));
  ASSERT_TRUE(res_drop.ok());
}

TEST_F(PerfettoSqlEngineTest, Table_Duplicates) {
  auto res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo AS SELECT 1 as bar"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();

  res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo AS SELECT 1 as bar"));
  ASSERT_FALSE(res.ok());

  res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE OR REPLACE PERFETTO TABLE foo AS SELECT 1 as bar"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlEngineTest, View_Create) {
  auto res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO VIEW foo AS SELECT 42 AS bar"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlEngineTest, View_Schema) {
  auto res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO VIEW foo(bar INT) AS SELECT 42 AS bar"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();

  res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO VIEW foo2(bar INT) AS SELECT 42 AS bar; SELECT 1"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlEngineTest, View_Drop) {
  auto res_create = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO VIEW foo AS SELECT 'foo' AS bar"));
  ASSERT_TRUE(res_create.ok());

  auto res_drop = engine_.Execute(SqlSource::FromExecuteQuery("DROP VIEW foo"));
  ASSERT_TRUE(res_drop.ok());
}

TEST_F(PerfettoSqlEngineTest, View_IncorrectSchema) {
  auto res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO VIEW foo(x INT) AS SELECT 1 as y"));
  ASSERT_FALSE(res.ok());
  EXPECT_THAT(
      res.status().c_message(),
      testing::EndsWith("CREATE PERFETTO VIEW: the following columns are "
                        "declared in the schema, but do not exist: x; and the "
                        "folowing columns exist, but are not declared: y"));
}

TEST_F(PerfettoSqlEngineTest, View_Duplicates) {
  auto res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO VIEW foo AS SELECT 1 as bar"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();

  res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO VIEW foo AS SELECT 1 as bar"));
  ASSERT_FALSE(res.ok());

  res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE OR REPLACE PERFETTO VIEW foo AS SELECT 1 as bar"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlEngineTest, Macro_Create) {
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

TEST_F(PerfettoSqlEngineTest, Macro_Duplicates) {
  auto res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO MACRO foo() RETURNS TableOrSubquery AS select 42 AS x"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();

  res = engine_.Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO MACRO foo() RETURNS TableOrSubquery AS select 42 AS x"));
  ASSERT_FALSE(res.ok());

  res = engine_.Execute(
      SqlSource::FromExecuteQuery("CREATE OR REPLACE PERFETTO MACRO foo() "
                                  "RETURNS TableOrSubquery AS select 42 AS x"));
  ASSERT_TRUE(res.ok());
}

TEST_F(PerfettoSqlEngineTest, Include_All) {
  engine_.RegisterPackage(
      "foo", CreateTestPackage(
                 {{"foo.foo", "CREATE PERFETTO TABLE foo AS SELECT 42 AS x"}}));
  engine_.RegisterPackage(
      "bar",
      CreateTestPackage(
          {{"bar.bar", "CREATE PERFETTO TABLE bar AS SELECT 42 AS x "}}));

  auto res_create =
      engine_.Execute(SqlSource::FromExecuteQuery("INCLUDE PERFETTO MODULE *"));
  ASSERT_TRUE(res_create.ok()) << res_create.status().c_message();
  ASSERT_TRUE(engine_.FindPackage("foo")->modules["foo.foo"].included);
  ASSERT_TRUE(engine_.FindPackage("bar")->modules["bar.bar"].included);
}

TEST_F(PerfettoSqlEngineTest, Include_Module) {
  engine_.RegisterPackage(
      "foo", CreateTestPackage({
                 {"foo.foo1", "CREATE PERFETTO TABLE foo1 AS SELECT 42 AS x"},
                 {"foo.foo2", "CREATE PERFETTO TABLE foo2 AS SELECT 42 AS x"},
             }));
  engine_.RegisterPackage(
      "bar",
      CreateTestPackage(
          {{"bar.bar", "CREATE PERFETTO TABLE bar AS SELECT 42 AS x "}}));

  auto res_create = engine_.Execute(
      SqlSource::FromExecuteQuery("INCLUDE PERFETTO MODULE foo.*"));
  ASSERT_TRUE(res_create.ok()) << res_create.status().c_message();
  ASSERT_TRUE(engine_.FindPackage("foo")->modules["foo.foo1"].included);
  ASSERT_TRUE(engine_.FindPackage("foo")->modules["foo.foo2"].included);
  ASSERT_FALSE(engine_.FindPackage("bar")->modules["bar.bar"].included);
}

TEST_F(PerfettoSqlEngineTest, MismatchedRange) {
  tables::SliceTable parent(&pool_);
  tables::ExpectedFrameTimelineSliceTable child(&pool_, &parent);

  engine_.RegisterStaticTable(&parent, "parent",
                              tables::SliceTable::ComputeStaticSchema());
  engine_.RegisterStaticTable(
      &child, "child",
      tables::ExpectedFrameTimelineSliceTable::ComputeStaticSchema());

  for (uint32_t i = 0; i < 5; i++) {
    child.Insert({});
  }

  for (uint32_t i = 0; i < 10; i++) {
    parent.Insert({});
  }

  auto res = engine_.Execute(
      SqlSource::FromExecuteQuery("SELECT * FROM child WHERE ts > 3"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
