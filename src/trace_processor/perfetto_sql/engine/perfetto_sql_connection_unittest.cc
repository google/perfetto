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

#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/util/sql_modules.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

using base::gtest_matchers::IsError;

class PerfettoSqlConnectionTest : public ::testing::Test {
 protected:
  StringPool pool_;
  std::unique_ptr<PerfettoSqlConnection> connection_ =
      PerfettoSqlConnection::CreateConnectionToNewDatabase(&pool_, true);
};

// Takes const char* literals (static storage) — RegisteredPackage stores
// string_views, so the bodies must outlive every connection that holds it.
sql_modules::RegisteredPackage CreateTestPackage(
    std::initializer_list<std::pair<const char*, const char*>> files) {
  sql_modules::RegisteredPackage result;
  for (const auto& file : files) {
    result.modules.Insert(file.first, std::string_view(file.second));
  }
  return result;
}

// These are the smoke tests for the perfetto SQL connection, focusing on
// ensuring that the correct statements do not return an error and that
// incorrect statements do.
//
// Functional tests are covered by the diff tests in
// test/trace_processor/diff_tests/syntax/perfetto_sql.

TEST_F(PerfettoSqlConnectionTest, Function_Create) {
  auto res = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO FUNCTION foo() RETURNS INT AS select 1"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();

  res = connection_->Execute(
      SqlSource::FromExecuteQuery("creatE PeRfEttO FUNCTION foo(x INT, y LONG) "
                                  "RETURNS INT AS select $x + $y"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlConnectionTest, Function_CreateWithArgs) {
  auto res = connection_->ExecuteUntilLastStatement(
      SqlSource::FromExecuteQuery("creatE PeRfEttO FUNCTION foo(x INT, y LONG) "
                                  "RETURNS INT AS select $x + $y;"
                                  "SELECT foo(1, 2)"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  ASSERT_FALSE(res->stmt.IsDone());
  ASSERT_EQ(sqlite3_column_int64(res->stmt.sqlite_stmt(), 0), 3);
  ASSERT_FALSE(res->stmt.Step());
}

TEST_F(PerfettoSqlConnectionTest, Function_Invalid) {
  auto res = connection_->ExecuteUntilLastStatement(
      SqlSource::FromExecuteQuery("creatE PeRfEttO FUNCTION foo(x INT, y LONG) "
                                  "AS select $x + $y;"
                                  "SELECT foo(1, 2)"));
  ASSERT_FALSE(res.ok());
}

TEST_F(PerfettoSqlConnectionTest, Function_Duplicates) {
  auto res = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO FUNCTION foo() RETURNS INT AS SELECT 1"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();

  res = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO FUNCTION foo() RETURNS INT AS SELECT 2"));
  ASSERT_FALSE(res.ok());

  res = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE OR REPLACE PERFETTO FUNCTION foo() RETURNS INT AS SELECT 3"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlConnectionTest, TableFunction_Create) {
  auto res = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO FUNCTION foo() RETURNS TABLE(x INT) AS "
      "select 1 AS x"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlConnectionTest, TableFunction_Duplicates) {
  auto res = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO FUNCTION foo() RETURNS TABLE(x INT) AS "
      "select 1 AS x"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();

  res = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO FUNCTION foo() RETURNS TABLE(x INT) AS "
      "select 1 AS x"));
  ASSERT_FALSE(res.ok());

  res = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE OR REPLACE PERFETTO FUNCTION foo() RETURNS TABLE(x INT) AS "
      "select 2 AS x"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlConnectionTest, Table_Create) {
  auto res = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo AS SELECT 42 AS bar"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlConnectionTest, Table_StringColumns) {
  auto res = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo AS SELECT 'foo' AS bar"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlConnectionTest, Table_Schema) {
  auto res = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo(bar INT) AS SELECT 42 AS bar"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();

  res = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo2(bar INT) AS SELECT 42 AS bar; SELECT 1"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlConnectionTest, Table_Schema_EmptyTable) {
  // This test checks that the type checks correctly work on empty tables (and
  // that columns with no data do not default to "int").
  auto res = connection_->Execute(
      SqlSource::FromExecuteQuery("CREATE PERFETTO TABLE foo(bar STRING) AS "
                                  "SELECT 'bar' as bar WHERE bar = 'foo'"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlConnectionTest, Table_Schema_NullColumn) {
  // This test checks that the type checks correctly work on columns without
  // data (and that columns with no non-NULL data do not default to "int").
  auto res = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo(bar STRING) AS SELECT NULL as bar"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlConnectionTest, Table_IncorrectSchema_MissingColumn) {
  auto res = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo(x INT) AS SELECT 1 as y"));
  ASSERT_FALSE(res.ok());
  EXPECT_THAT(
      res.status().c_message(),
      testing::EndsWith("CREATE PERFETTO TABLE: the following columns are "
                        "declared in the schema, but do not exist: x; and the "
                        "following columns exist, but are not declared: y"));
}

TEST_F(PerfettoSqlConnectionTest, Table_IncorrectSchema_IncorrectType) {
  auto res = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo(x INT) AS SELECT '1' as x"));
  ASSERT_FALSE(res.ok());
  EXPECT_THAT(
      res.status().c_message(),
      testing::EndsWith("CREATE PERFETTO TABLE(foo): column 'x' declared as "
                        "LONG in the schema, but STRING found"));
}

TEST_F(PerfettoSqlConnectionTest, Table_Drop) {
  auto res_create = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo AS SELECT 'foo' AS bar"));
  ASSERT_TRUE(res_create.ok());

  auto res_drop =
      connection_->Execute(SqlSource::FromExecuteQuery("DROP TABLE foo"));
  ASSERT_TRUE(res_drop.ok());
}

TEST_F(PerfettoSqlConnectionTest, Table_Duplicates) {
  auto res = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo AS SELECT 1 as bar"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();

  res = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo AS SELECT 1 as bar"));
  ASSERT_FALSE(res.ok());

  res = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE OR REPLACE PERFETTO TABLE foo AS SELECT 1 as bar"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlConnectionTest, View_Create) {
  auto res = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO VIEW foo AS SELECT 42 AS bar"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlConnectionTest, View_Schema) {
  auto res = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO VIEW foo(bar INT) AS SELECT 42 AS bar"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();

  res = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO VIEW foo2(bar INT) AS SELECT 42 AS bar; SELECT 1"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlConnectionTest, View_Drop) {
  auto res_create = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO VIEW foo AS SELECT 'foo' AS bar"));
  ASSERT_TRUE(res_create.ok());

  auto res_drop =
      connection_->Execute(SqlSource::FromExecuteQuery("DROP VIEW foo"));
  ASSERT_TRUE(res_drop.ok());
}

TEST_F(PerfettoSqlConnectionTest, View_IncorrectSchema) {
  auto res = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO VIEW foo(x INT) AS SELECT 1 as y"));
  ASSERT_FALSE(res.ok());
  EXPECT_THAT(
      res.status().c_message(),
      testing::EndsWith("CREATE PERFETTO VIEW: the following columns are "
                        "declared in the schema, but do not exist: x; and the "
                        "following columns exist, but are not declared: y"));
}

TEST_F(PerfettoSqlConnectionTest, View_Duplicates) {
  auto res = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO VIEW foo AS SELECT 1 as bar"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();

  res = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO VIEW foo AS SELECT 1 as bar"));
  ASSERT_FALSE(res.ok());

  res = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE OR REPLACE PERFETTO VIEW foo AS SELECT 1 as bar"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlConnectionTest, Macro_Create) {
  auto res_create = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO MACRO foo() RETURNS TableOrSubquery AS select 42 AS x"));
  ASSERT_TRUE(res_create.ok()) << res_create.status().c_message();

  res_create = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO MACRO bar(x TableOrSubquery) RETURNS TableOrSubquery AS "
      "select * from $x"));
  ASSERT_TRUE(res_create.ok()) << res_create.status().c_message();

  auto res = connection_->ExecuteUntilLastStatement(
      SqlSource::FromExecuteQuery("bar!((foo!()))"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  ASSERT_FALSE(res->stmt.IsDone());
  ASSERT_EQ(sqlite3_column_int64(res->stmt.sqlite_stmt(), 0), 42);
  ASSERT_FALSE(res->stmt.Step());
}

TEST_F(PerfettoSqlConnectionTest, Macro_Duplicates) {
  auto res = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO MACRO foo() RETURNS TableOrSubquery AS select 42 AS x"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();

  res = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO MACRO foo() RETURNS TableOrSubquery AS select 42 AS x"));
  ASSERT_FALSE(res.ok());

  res = connection_->Execute(
      SqlSource::FromExecuteQuery("CREATE OR REPLACE PERFETTO MACRO foo() "
                                  "RETURNS TableOrSubquery AS select 42 AS x"));
  ASSERT_TRUE(res.ok());
}

TEST_F(PerfettoSqlConnectionTest, Include_All) {
  connection_->RegisterPackage(
      "foo", CreateTestPackage(
                 {{"foo.foo", "CREATE PERFETTO TABLE foo AS SELECT 42 AS x"}}));
  connection_->RegisterPackage(
      "bar",
      CreateTestPackage(
          {{"bar.bar", "CREATE PERFETTO TABLE bar AS SELECT 42 AS x "}}));

  auto res_create = connection_->Execute(
      SqlSource::FromExecuteQuery("INCLUDE PERFETTO MODULE *"));
  ASSERT_TRUE(res_create.ok()) << res_create.status().c_message();
  auto* db = connection_->database_for_testing();
  ASSERT_TRUE(db->IsModuleIncluded("foo.foo"));
  ASSERT_TRUE(db->IsModuleIncluded("bar.bar"));
}

TEST_F(PerfettoSqlConnectionTest, Include_Module) {
  connection_->RegisterPackage(
      "foo", CreateTestPackage({
                 {"foo.foo1", "CREATE PERFETTO TABLE foo1 AS SELECT 42 AS x"},
                 {"foo.foo2", "CREATE PERFETTO TABLE foo2 AS SELECT 42 AS x"},
             }));
  connection_->RegisterPackage(
      "bar",
      CreateTestPackage(
          {{"bar.bar", "CREATE PERFETTO TABLE bar AS SELECT 42 AS x "}}));

  auto res_create = connection_->Execute(
      SqlSource::FromExecuteQuery("INCLUDE PERFETTO MODULE foo.*"));
  ASSERT_TRUE(res_create.ok()) << res_create.status().c_message();
  auto* db = connection_->database_for_testing();
  ASSERT_TRUE(db->IsModuleIncluded("foo.foo1"));
  ASSERT_TRUE(db->IsModuleIncluded("foo.foo2"));
  ASSERT_FALSE(db->IsModuleIncluded("bar.bar"));
}

TEST_F(PerfettoSqlConnectionTest, Include_FailingModulePoisons) {
  // A module with a syntactically broken body. The first INCLUDE fails;
  // subsequent INCLUDEs of the same key short-circuit with a poison
  // error rather than re-running the broken body.
  connection_->RegisterPackage(
      "foo", CreateTestPackage({{"foo.bad", "this is not valid sql"}}));

  ASSERT_THAT(connection_->Execute(SqlSource::FromExecuteQuery(
                  "INCLUDE PERFETTO MODULE foo.bad")),
              IsError());
  ASSERT_TRUE(connection_->database_for_testing()->IsModulePoisoned("foo.bad"));

  auto second = connection_->Execute(
      SqlSource::FromExecuteQuery("INCLUDE PERFETTO MODULE foo.bad"));
  ASSERT_THAT(second, IsError());
  EXPECT_THAT(second.status().c_message(),
              testing::HasSubstr("poisoned by earlier failure"));
}

TEST_F(PerfettoSqlConnectionTest, Include_AlreadyIncludedShortCircuits) {
  // A module body whose successful execution has a side-effect (CREATE
  // TABLE). The second INCLUDE must NOT re-run the body — if it did,
  // SQLite would return "table already exists" and the second INCLUDE
  // would fail.
  connection_->RegisterPackage(
      "foo", CreateTestPackage(
                 {{"foo.t", "CREATE PERFETTO TABLE foo AS SELECT 42 AS x"}}));

  ASSERT_OK(connection_->Execute(
      SqlSource::FromExecuteQuery("INCLUDE PERFETTO MODULE foo.t")));
  ASSERT_OK(connection_->Execute(
      SqlSource::FromExecuteQuery("INCLUDE PERFETTO MODULE foo.t")));
}

TEST_F(PerfettoSqlConnectionTest, Include_PoisonPropagatesUpStack) {
  // foo.bad's body is broken; foo.good's body INCLUDEs foo.bad. Including
  // foo.good fails, and BOTH foo.bad AND foo.good should be poisoned —
  // poisoning walks up the stack as include frames are unwound on error.
  connection_->RegisterPackage(
      "foo",
      CreateTestPackage({{"foo.bad", "this is not valid sql"},
                         {"foo.good", "INCLUDE PERFETTO MODULE foo.bad"}}));

  ASSERT_THAT(connection_->Execute(SqlSource::FromExecuteQuery(
                  "INCLUDE PERFETTO MODULE foo.good")),
              IsError());

  auto* db = connection_->database_for_testing();
  ASSERT_TRUE(db->IsModulePoisoned("foo.bad"));
  ASSERT_TRUE(db->IsModulePoisoned("foo.good"));
}

TEST_F(PerfettoSqlConnectionTest,
       Include_PoisonError_Direct_HasImmediateTraceback) {
  // Re-including a poisoned module via a direct INCLUDE statement should
  // surface the immediate INCLUDE line in the error traceback, matching the
  // error format produced by the asynchronous failure path.
  connection_->RegisterPackage(
      "foo", CreateTestPackage({{"foo.bad", "this is not valid sql"}}));
  ASSERT_THAT(connection_->Execute(SqlSource::FromExecuteQuery(
                  "INCLUDE PERFETTO MODULE foo.bad")),
              IsError());

  auto retry = connection_->Execute(
      SqlSource::FromExecuteQuery("INCLUDE PERFETTO MODULE foo.bad"));
  ASSERT_THAT(retry, IsError());
  EXPECT_THAT(retry.status().c_message(),
              testing::HasSubstr("INCLUDE PERFETTO MODULE foo.bad"));
  EXPECT_THAT(retry.status().c_message(),
              testing::HasSubstr("poisoned by earlier failure"));
}

TEST_F(PerfettoSqlConnectionTest,
       Include_PoisonError_Wildcard_HasImmediateTraceback) {
  // Same as above but the retry happens via a wildcard expansion. The
  // wildcard's own statement line should appear in the traceback.
  connection_->RegisterPackage(
      "foo", CreateTestPackage({{"foo.bad", "this is not valid sql"}}));
  ASSERT_THAT(connection_->Execute(SqlSource::FromExecuteQuery(
                  "INCLUDE PERFETTO MODULE foo.bad")),
              IsError());

  auto retry = connection_->Execute(
      SqlSource::FromExecuteQuery("INCLUDE PERFETTO MODULE foo.*"));
  ASSERT_THAT(retry, IsError());
  EXPECT_THAT(retry.status().c_message(),
              testing::HasSubstr("INCLUDE PERFETTO MODULE foo.*"));
  EXPECT_THAT(retry.status().c_message(),
              testing::HasSubstr("poisoned by earlier failure"));
}

TEST_F(PerfettoSqlConnectionTest, Include_PoisonReason_StableAcrossRetries) {
  // Each retry of a poisoned include must produce an error message of the
  // same length — the stored poison reason should not be padded with the
  // retry's own traceback layers each time.
  connection_->RegisterPackage(
      "foo", CreateTestPackage({{"foo.bad", "this is not valid sql"}}));
  ASSERT_THAT(connection_->Execute(SqlSource::FromExecuteQuery(
                  "INCLUDE PERFETTO MODULE foo.bad")),
              IsError());

  auto a = connection_->Execute(
      SqlSource::FromExecuteQuery("INCLUDE PERFETTO MODULE foo.bad"));
  auto b = connection_->Execute(
      SqlSource::FromExecuteQuery("INCLUDE PERFETTO MODULE foo.bad"));
  ASSERT_THAT(a, IsError());
  ASSERT_THAT(b, IsError());
  EXPECT_EQ(a.status().message().size(), b.status().message().size());
}

TEST_F(PerfettoSqlConnectionTest, Include_CycleDetected) {
  // foo.a includes foo.b, foo.b includes foo.a. Walking the connection's
  // own execution stack catches the re-entry and surfaces a cycle error
  // rather than re-entering |TryClaimInclude| (which would deadlock on its
  // own claim).
  connection_->RegisterPackage("foo",
                               CreateTestPackage({
                                   {"foo.a", "INCLUDE PERFETTO MODULE foo.b"},
                                   {"foo.b", "INCLUDE PERFETTO MODULE foo.a"},
                               }));

  auto res = connection_->Execute(
      SqlSource::FromExecuteQuery("INCLUDE PERFETTO MODULE foo.a"));
  ASSERT_THAT(res, IsError());
  EXPECT_THAT(res.status().c_message(), testing::HasSubstr("cycle detected"));
}

TEST_F(PerfettoSqlConnectionTest, RegisterPackage_FailsAfterModuleIncluded) {
  // Re-registering a package whose module has already been imported is
  // rejected: silently shadowing the imported body would surprise callers
  // that have built on top of the original SQL.
  ASSERT_OK(connection_->RegisterPackage(
      "foo", CreateTestPackage(
                 {{"foo.t", "CREATE PERFETTO TABLE foo AS SELECT 42 AS x"}})));
  ASSERT_OK(connection_->Execute(
      SqlSource::FromExecuteQuery("INCLUDE PERFETTO MODULE foo.t")));

  auto status = connection_->RegisterPackage(
      "foo", CreateTestPackage(
                 {{"foo.t", "CREATE PERFETTO TABLE bar AS SELECT 1 AS x"}}));
  ASSERT_THAT(status, IsError());
  EXPECT_THAT(status.c_message(), testing::HasSubstr("foo.t"));
  EXPECT_THAT(status.c_message(), testing::HasSubstr("already been included"));
}

TEST_F(PerfettoSqlConnectionTest, RegisterPackage_FailsAfterModulePoisoned) {
  // Likewise for the poisoned case.
  ASSERT_OK(connection_->RegisterPackage(
      "foo", CreateTestPackage({{"foo.bad", "this is not valid sql"}})));
  ASSERT_THAT(connection_->Execute(SqlSource::FromExecuteQuery(
                  "INCLUDE PERFETTO MODULE foo.bad")),
              IsError());

  auto status = connection_->RegisterPackage(
      "foo", CreateTestPackage({{"foo.bad", "SELECT 1"}}));
  ASSERT_THAT(status, IsError());
  EXPECT_THAT(status.c_message(), testing::HasSubstr("foo.bad"));
  EXPECT_THAT(status.c_message(), testing::HasSubstr("poisoned"));
}

TEST_F(PerfettoSqlConnectionTest, DelegatingFunction_Error_TargetNotFound) {
  // Test error when target function doesn't exist in registry
  auto res = connection_->Execute(
      SqlSource::FromExecuteQuery("CREATE PERFETTO FUNCTION my_alias() RETURNS "
                                  "INT DELEGATES TO nonexistent_func"));
  ASSERT_FALSE(res.ok());
  EXPECT_THAT(res.status().c_message(),
              testing::HasSubstr(
                  "Target function 'nonexistent_func' not found in registry"));
}

TEST_F(PerfettoSqlConnectionTest, DelegatingFunction_Error_ReplaceRequired) {
  // First, we need to register a target function for aliasing
  // Since we can't easily access the intrinsic registry in tests,
  // let's test the replace logic with a regular function first
  auto res1 = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO FUNCTION test_func() RETURNS INT AS SELECT 42"));
  ASSERT_TRUE(res1.ok()) << res1.status().c_message();

  // Try to create same function without replace - should fail
  auto res2 = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE PERFETTO FUNCTION test_func() RETURNS INT AS SELECT 43"));
  ASSERT_FALSE(res2.ok());
  EXPECT_THAT(res2.status().c_message(),
              testing::HasSubstr("function already exists"));

  // Try with OR REPLACE - should succeed
  auto res3 = connection_->Execute(
      SqlSource::FromExecuteQuery("CREATE OR REPLACE PERFETTO FUNCTION "
                                  "test_func() RETURNS INT AS SELECT 44"));
  ASSERT_TRUE(res3.ok()) << res3.status().c_message();
}

TEST(SqlModulesTest, IsPackagePrefixOf) {
  // Exact match
  EXPECT_TRUE(sql_modules::IsPackagePrefixOf("foo", "foo"));
  EXPECT_TRUE(sql_modules::IsPackagePrefixOf("foo.bar", "foo.bar"));

  // Proper prefix (followed by dot)
  EXPECT_TRUE(sql_modules::IsPackagePrefixOf("foo", "foo.bar"));
  EXPECT_TRUE(sql_modules::IsPackagePrefixOf("foo.bar", "foo.bar.baz"));

  // Not a prefix (no dot separator)
  EXPECT_FALSE(sql_modules::IsPackagePrefixOf("foo", "foobar"));
  EXPECT_FALSE(sql_modules::IsPackagePrefixOf("foo.bar", "foo.barbaz"));

  // Prefix longer than string
  EXPECT_FALSE(sql_modules::IsPackagePrefixOf("foo.bar", "foo"));
  EXPECT_FALSE(sql_modules::IsPackagePrefixOf("foo.bar.baz", "foo.bar"));
}

TEST_F(PerfettoSqlConnectionTest, FindPackageForModule_MultiLevel) {
  // Register a multi-level package
  connection_->RegisterPackage(
      "foo.bar",
      CreateTestPackage(
          {{"foo.bar.baz", "CREATE PERFETTO TABLE t AS SELECT 1 AS x"}}));

  // FindPackageForModule should find it
  EXPECT_NE(connection_->FindPackageForModule("foo.bar.baz"), nullptr);
  EXPECT_NE(connection_->FindPackageForModule("foo.bar.baz.qux"), nullptr);

  // But not for unrelated modules
  EXPECT_EQ(connection_->FindPackageForModule("foo.other"), nullptr);
  EXPECT_EQ(connection_->FindPackageForModule("other.bar"), nullptr);
}

// ExecuteNextStatement tests. These simulate the caller-side cursor loop:
// pass the full SQL each time, substr'd at the offset from the previous call.

TEST_F(PerfettoSqlConnectionTest, NextStatement_EveryResultSetReturned) {
  std::string sql = "SELECT 1; SELECT 2";
  auto src = SqlSource::FromExecuteQuery(sql);
  auto size = static_cast<uint32_t>(sql.size());

  uint32_t end = 0;
  auto res = connection_->ExecuteNextStatement(src.Substr(0, size), &end);
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  ASSERT_TRUE(res->has_value());
  ASSERT_FALSE((*res)->stmt.IsDone());
  ASSERT_EQ(sqlite3_column_int64((*res)->stmt.sqlite_stmt(), 0), 1);
  ASSERT_FALSE((*res)->stmt.Step());
  ASSERT_EQ((*res)->stats.statement_count, 1u);
  ASSERT_GT(end, 0u);
  ASSERT_LT(end, size);

  uint32_t start = end;
  end = 0;
  res =
      connection_->ExecuteNextStatement(src.Substr(start, size - start), &end);
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  ASSERT_TRUE(res->has_value());
  ASSERT_FALSE((*res)->stmt.IsDone());
  ASSERT_EQ(sqlite3_column_int64((*res)->stmt.sqlite_stmt(), 0), 2);
  ASSERT_FALSE((*res)->stmt.Step());
  ASSERT_EQ(start + end, size);

  res =
      connection_->ExecuteNextStatement(SqlSource::FromExecuteQuery(""), &end);
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  ASSERT_FALSE(res->has_value());
  ASSERT_EQ(end, 0u);
}

TEST_F(PerfettoSqlConnectionTest, NextStatement_StatePersistsAcrossCalls) {
  std::string sql =
      "CREATE PERFETTO MACRO life() RETURNS Expr AS 42;"
      "CREATE PERFETTO TABLE t AS SELECT life!() AS x;"
      "SELECT x FROM t";
  auto src = SqlSource::FromExecuteQuery(sql);
  auto size = static_cast<uint32_t>(sql.size());

  uint32_t offset = 0;
  for (int i = 0; i < 2; ++i) {
    uint32_t end = 0;
    auto res = connection_->ExecuteNextStatement(
        src.Substr(offset, size - offset), &end);
    ASSERT_TRUE(res.ok()) << res.status().c_message();
    ASSERT_TRUE(res->has_value());
    ASSERT_TRUE((*res)->stmt.IsDone());
    offset += end;
  }

  uint32_t end = 0;
  auto res = connection_->ExecuteNextStatement(
      src.Substr(offset, size - offset), &end);
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  ASSERT_TRUE(res->has_value());
  ASSERT_FALSE((*res)->stmt.IsDone());
  ASSERT_EQ(sqlite3_column_int64((*res)->stmt.sqlite_stmt(), 0), 42);
}

TEST_F(PerfettoSqlConnectionTest, NextStatement_CommentsOnly) {
  std::string sql = "-- comment\n/* another */";
  uint32_t end = 0;
  auto res =
      connection_->ExecuteNextStatement(SqlSource::FromExecuteQuery(sql), &end);
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  ASSERT_FALSE(res->has_value());
  ASSERT_EQ(end, static_cast<uint32_t>(sql.size()));
}

TEST_F(PerfettoSqlConnectionTest, NextStatement_Include) {
  connection_->RegisterPackage(
      "foo", CreateTestPackage(
                 {{"foo.foo", "CREATE PERFETTO TABLE foo AS SELECT 42 AS x"}}));

  std::string sql = "INCLUDE PERFETTO MODULE foo.foo; SELECT x FROM foo";
  auto src = SqlSource::FromExecuteQuery(sql);
  auto size = static_cast<uint32_t>(sql.size());

  uint32_t end = 0;
  auto res = connection_->ExecuteNextStatement(src.Substr(0, size), &end);
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  ASSERT_TRUE(res->has_value());
  ASSERT_TRUE((*res)->stmt.IsDone());
  ASSERT_TRUE(connection_->database_for_testing()->IsModuleIncluded("foo.foo"));

  uint32_t start = end;
  res =
      connection_->ExecuteNextStatement(src.Substr(start, size - start), &end);
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  ASSERT_TRUE(res->has_value());
  ASSERT_FALSE((*res)->stmt.IsDone());
  ASSERT_EQ(sqlite3_column_int64((*res)->stmt.sqlite_stmt(), 0), 42);
}

TEST_F(PerfettoSqlConnectionTest, NextStatement_Error) {
  uint32_t end = 0;
  auto res = connection_->ExecuteNextStatement(
      SqlSource::FromExecuteQuery("SELECT * FROM not_a_table; SELECT 1"), &end);
  ASSERT_THAT(res, IsError());
}

TEST_F(PerfettoSqlConnectionTest, NextStatement_ErrorInLaterStatement) {
  std::string sql = "SELECT 1; SELECT * FROM not_a_table";
  auto src = SqlSource::FromExecuteQuery(sql);
  auto size = static_cast<uint32_t>(sql.size());

  uint32_t end = 0;
  auto res = connection_->ExecuteNextStatement(src.Substr(0, size), &end);
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  ASSERT_TRUE(res->has_value());
  ASSERT_EQ(sqlite3_column_int64((*res)->stmt.sqlite_stmt(), 0), 1);

  // The second statement fails to prepare; |end| must be left untouched.
  uint32_t start = end;
  end = 0xdeadbeef;
  res =
      connection_->ExecuteNextStatement(src.Substr(start, size - start), &end);
  ASSERT_THAT(res, IsError());
  ASSERT_EQ(end, 0xdeadbeef);

  // The connection must remain usable after the mid-loop error.
  end = 0;
  res = connection_->ExecuteNextStatement(
      SqlSource::FromExecuteQuery("SELECT 3"), &end);
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  ASSERT_TRUE(res->has_value());
  ASSERT_EQ(sqlite3_column_int64((*res)->stmt.sqlite_stmt(), 0), 3);
}

TEST_F(PerfettoSqlConnectionTest, NextStatement_TrailingSemicolon) {
  std::string sql = "SELECT 1;";
  auto src = SqlSource::FromExecuteQuery(sql);
  auto size = static_cast<uint32_t>(sql.size());

  uint32_t end = 0;
  auto res = connection_->ExecuteNextStatement(src.Substr(0, size), &end);
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  ASSERT_TRUE(res->has_value());
  ASSERT_EQ(sqlite3_column_int64((*res)->stmt.sqlite_stmt(), 0), 1);

  // The leftover tail (";" or "") must report exhaustion, not loop or error.
  uint32_t start = end;
  end = 0;
  res =
      connection_->ExecuteNextStatement(src.Substr(start, size - start), &end);
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  ASSERT_FALSE(res->has_value());
  ASSERT_EQ(start + end, size);
}

TEST_F(PerfettoSqlConnectionTest, NextStatement_ZeroRowResultSet) {
  std::string sql = "SELECT 1 AS a WHERE 0";
  uint32_t end = 0;
  auto res =
      connection_->ExecuteNextStatement(SqlSource::FromExecuteQuery(sql), &end);
  ASSERT_TRUE(res.ok()) << res.status().c_message();

  // Zero rows is still a result set: the statement must be returned (not
  // classified as no-statement) with its column intact.
  ASSERT_TRUE(res->has_value());
  ASSERT_TRUE((*res)->stmt.IsDone());
  ASSERT_EQ((*res)->stats.column_count, 1u);
  ASSERT_EQ(end, static_cast<uint32_t>(sql.size()));
}

TEST_F(PerfettoSqlConnectionTest, NextStatement_TrailingDummyStatement) {
  std::string sql = "SELECT 1; CREATE PERFETTO TABLE u AS SELECT 2 AS x";
  auto src = SqlSource::FromExecuteQuery(sql);
  auto size = static_cast<uint32_t>(sql.size());

  uint32_t end = 0;
  auto res = connection_->ExecuteNextStatement(src.Substr(0, size), &end);
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  ASSERT_TRUE(res->has_value());
  ASSERT_EQ(sqlite3_column_int64((*res)->stmt.sqlite_stmt(), 0), 1);

  // The trailing transpiled statement executes via a dummy: it must not leak
  // the dummy's phantom column.
  uint32_t start = end;
  end = 0;
  res =
      connection_->ExecuteNextStatement(src.Substr(start, size - start), &end);
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  ASSERT_TRUE(res->has_value());
  ASSERT_TRUE((*res)->stmt.IsDone());
  ASSERT_EQ((*res)->stats.column_count, 0u);
  ASSERT_EQ(start + end, size);

  uint32_t unused = 0;
  res = connection_->ExecuteNextStatement(
      SqlSource::FromExecuteQuery("SELECT x FROM u"), &unused);
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  ASSERT_TRUE(res->has_value());
  ASSERT_EQ(sqlite3_column_int64((*res)->stmt.sqlite_stmt(), 0), 2);
}

TEST_F(PerfettoSqlConnectionTest, PipelinePragmaEnableInBatch) {
  auto res = connection_->Execute(SqlSource::FromExecuteQuery(
      "PERFETTO PRAGMA pipelines = 1; FROM (SELECT 1 AS x)"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
}

TEST_F(PerfettoSqlConnectionTest, PipelinePragmaDisableInBatch) {
  ASSERT_TRUE(connection_
                  ->Execute(SqlSource::FromExecuteQuery(
                      "PERFETTO PRAGMA pipelines = 1"))
                  .ok());
  auto res = connection_->Execute(SqlSource::FromExecuteQuery(
      "PERFETTO PRAGMA pipelines = 0; FROM (SELECT 1 AS x)"));
  ASSERT_FALSE(res.ok());
  EXPECT_THAT(res.status().message(),
              testing::HasSubstr("Pipelines are not enabled"));
}

TEST_F(PerfettoSqlConnectionTest, PipelinePragmaIncludeChangesCaller) {
  ASSERT_OK(connection_->RegisterPackage(
      "foo", CreateTestPackage(
                 {{"foo.enable",
                   "PERFETTO PRAGMA pipelines = 1; "
                   "CREATE PERFETTO TABLE enabled AS FROM (SELECT 1 AS x)"},
                  {"foo.disable", "PERFETTO PRAGMA pipelines = 0"}})));
  auto res = connection_->Execute(SqlSource::FromExecuteQuery(
      "INCLUDE PERFETTO MODULE foo.enable; FROM (SELECT 1 AS x)"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  res = connection_->Execute(SqlSource::FromExecuteQuery(
      "INCLUDE PERFETTO MODULE foo.disable; FROM (SELECT 1 AS x)"));
  ASSERT_FALSE(res.ok());
  EXPECT_THAT(res.status().message(),
              testing::HasSubstr("Pipelines are not enabled"));
}

TEST_F(PerfettoSqlConnectionTest, PipelinePragmaBuiltinExemption) {
  auto package = CreateTestPackage(
      {{"foo.pipeline",
        "PERFETTO PRAGMA pipelines = 0; "
        "CREATE PERFETTO TABLE builtin AS FROM (SELECT 1 AS x)"}});
  package.builtin = true;
  ASSERT_OK(connection_->RegisterPackage("foo", std::move(package)));
  auto res = connection_->Execute(SqlSource::FromExecuteQuery(
      "INCLUDE PERFETTO MODULE foo.*; FROM (SELECT 1 AS x)"));
  ASSERT_FALSE(res.ok());
  EXPECT_TRUE(
      connection_->database_for_testing()->IsModuleIncluded("foo.pipeline"));
  EXPECT_THAT(res.status().message(),
              testing::HasSubstr("Pipelines are not enabled"));
}

class PerfettoSqlConnectionPipelineTest : public PerfettoSqlConnectionTest {
 protected:
  void SetUp() override {
    auto res = connection_->Execute(SqlSource::FromExecuteQuery(R"(
      PERFETTO PRAGMA pipelines = 1;
      CREATE TABLE tree(id INTEGER, parent_id INTEGER, self INTEGER,
                        name TEXT);
      INSERT INTO tree VALUES
        (0, NULL, 10, 'root'), (1, 0, 20, 'a'), (2, 0, 30, NULL),
        (3, 1, 40, 'c');
    )"));
    ASSERT_TRUE(res.ok()) << res.status().c_message();
  }

  // Runs `sql` and returns the rows of the last statement as text, sorted.
  base::StatusOr<std::vector<std::string>> Rows(const std::string& sql) {
    auto res = connection_->ExecuteUntilLastStatement(
        SqlSource::FromExecuteQuery(sql));
    RETURN_IF_ERROR(res.status());
    std::vector<std::string> rows;
    sqlite3_stmt* stmt = res->stmt.sqlite_stmt();
    for (bool more = !res->stmt.IsDone(); more; more = res->stmt.Step()) {
      std::string row;
      for (int i = 0; i < sqlite3_column_count(stmt); ++i) {
        const auto* text =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
        row += (i ? "," : "") + std::string(text ? text : "NULL");
      }
      rows.push_back(std::move(row));
    }
    RETURN_IF_ERROR(res->stmt.status());
    std::sort(rows.begin(), rows.end());
    return rows;
  }

  std::vector<std::string> ColumnNames(const std::string& sql) {
    auto res = connection_->ExecuteUntilLastStatement(
        SqlSource::FromExecuteQuery(sql));
    PERFETTO_CHECK(res.ok());
    std::vector<std::string> names;
    for (uint32_t i = 0; i < res->stats.column_count; ++i) {
      names.push_back(
          sqlite3_column_name(res->stmt.sqlite_stmt(), static_cast<int>(i)));
    }
    return names;
  }
};

TEST_F(PerfettoSqlConnectionPipelineTest, AccumulateUpAndDown) {
  const char kQuery[] = R"(
    FROM tree
    |> TREE ACCUMULATE UP SUM(self) AS total
    |> TREE ACCUMULATE DOWN SUM(self) AS path
  )";
  EXPECT_THAT(
      ColumnNames(kQuery),
      testing::ElementsAre("id", "parent_id", "self", "name", "total", "path"));
  auto rows = Rows(kQuery);
  ASSERT_TRUE(rows.ok()) << rows.status().c_message();
  EXPECT_THAT(*rows,
              testing::ElementsAre("0,NULL,10,root,100,10", "1,0,20,a,60,30",
                                   "2,0,30,NULL,30,40", "3,1,40,c,40,70"));
}

TEST_F(PerfettoSqlConnectionPipelineTest, StartsFromAnySql) {
  auto rows = Rows(R"(
    FROM (SELECT id, parent_id, self * 2 AS doubled FROM tree WHERE id < 10)
    |> TREE ACCUMULATE UP SUM(doubled) AS total
  )");
  ASSERT_TRUE(rows.ok()) << rows.status().c_message();
  EXPECT_THAT(*rows, testing::ElementsAre("0,NULL,20,200", "1,0,40,120",
                                          "2,0,60,60", "3,1,80,80"));
}

// Perfetto tables are dataframes and are scanned directly.
TEST_F(PerfettoSqlConnectionPipelineTest, StartsFromAPerfettoTable) {
  auto rows = Rows(R"(
    CREATE PERFETTO TABLE df AS SELECT * FROM tree;
    FROM df |> TREE ACCUMULATE UP SUM(self) AS total
  )");
  ASSERT_TRUE(rows.ok()) << rows.status().c_message();
  EXPECT_THAT(*rows, testing::ElementsAre("0,NULL,10,root,100", "1,0,20,a,60",
                                          "2,0,30,NULL,30", "3,1,40,c,40"));
}

TEST_F(PerfettoSqlConnectionPipelineTest, CreatePerfettoTableAsPipeline) {
  auto rows = Rows(R"(
    CREATE PERFETTO TABLE totals AS
    FROM tree |> TREE ACCUMULATE UP SUM(self) AS total;
    SELECT id, total FROM totals
  )");
  ASSERT_TRUE(rows.ok()) << rows.status().c_message();
  EXPECT_THAT(*rows, testing::ElementsAre("0,100", "1,60", "2,30", "3,40"));
}

TEST_F(PerfettoSqlConnectionPipelineTest, RunsBetweenOtherStatements) {
  auto rows = Rows(R"(
    FROM tree |> TREE ACCUMULATE UP SUM(self) AS total;
    FROM tree |> TREE ACCUMULATE DOWN SUM(self) AS path;
    SELECT count(*) FROM tree
  )");
  ASSERT_TRUE(rows.ok()) << rows.status().c_message();
  EXPECT_THAT(*rows, testing::ElementsAre("4"));
}

TEST_F(PerfettoSqlConnectionPipelineTest, ExpandsMacros) {
  auto rows = Rows(R"(
    CREATE PERFETTO MACRO leaves_of(t TableOrSubquery)
    RETURNS TableOrSubquery AS (SELECT * FROM $t WHERE id != 3);
    FROM leaves_of!(tree) |> TREE ACCUMULATE UP SUM(self) AS total
  )");
  ASSERT_TRUE(rows.ok()) << rows.status().c_message();
  EXPECT_THAT(*rows, testing::ElementsAre("0,NULL,10,root,60", "1,0,20,a,20",
                                          "2,0,30,NULL,30"));
}

TEST_F(PerfettoSqlConnectionPipelineTest, DuplicateOutputNames) {
  const char kQuery[] = "FROM tree |> TREE ACCUMULATE UP SUM(self) AS self";
  auto rows = Rows(kQuery);
  ASSERT_TRUE(rows.ok()) << rows.status().message();
  EXPECT_THAT(*rows, testing::ElementsAre("0,NULL,10,root,100", "1,0,20,a,60",
                                          "2,0,30,NULL,30", "3,1,40,c,40"));
  EXPECT_THAT(ColumnNames(kQuery),
              testing::ElementsAre("id", "parent_id", "self", "name", "self"));
  EXPECT_THAT(
      Rows(std::string(kQuery) + " |> TREE ACCUMULATE UP SUM(self) AS total")
          .status()
          .message(),
      testing::HasSubstr("column 'self' is ambiguous"));
}

TEST_F(PerfettoSqlConnectionPipelineTest, OutputNamesAreQuoted) {
  EXPECT_THAT(ColumnNames(R"(FROM (SELECT 1 AS "", 2 AS "a""b"))"),
              testing::ElementsAre("", "a\"b"));
  auto rows = Rows(R"(FROM (SELECT 1 AS "", 2 AS "a""b"))");
  ASSERT_TRUE(rows.ok()) << rows.status().message();
  EXPECT_THAT(*rows, testing::ElementsAre("1,2"));
}

TEST_F(PerfettoSqlConnectionPipelineTest, Errors) {
  EXPECT_THAT(Rows("FROM tree |> TREE ACCUMULATE UP SUM(nope) AS total")
                  .status()
                  .message(),
              testing::HasSubstr("no such column: 'nope'"));
  EXPECT_THAT(Rows("FROM tree |> TREE ACCUMULATE UP SUM(name) AS total")
                  .status()
                  .message(),
              testing::HasSubstr("'name'"));
  EXPECT_THAT(Rows("CREATE PERFETTO TABLE t AS FROM tree |> WHERE id = 1")
                  .status()
                  .message(),
              testing::HasSubstr("syntax error near 'WHERE'"));
}

// Replacing a table mid-read must not affect a running pipeline.
TEST_F(PerfettoSqlConnectionPipelineTest, AReadTableCanBeReplaced) {
  ASSERT_TRUE(connection_
                  ->Execute(SqlSource::FromExecuteQuery(
                      "CREATE PERFETTO TABLE df AS SELECT * FROM tree"))
                  .ok());
  auto res = connection_->ExecuteUntilLastStatement(SqlSource::FromExecuteQuery(
      "FROM df |> TREE ACCUMULATE UP SUM(self) AS total"));
  ASSERT_TRUE(res.ok()) << res.status().c_message();
  ASSERT_FALSE(res->stmt.IsDone());

  auto replace = connection_->Execute(SqlSource::FromExecuteQuery(
      "CREATE OR REPLACE PERFETTO TABLE df AS SELECT 1 AS id"));
  ASSERT_TRUE(replace.ok()) << replace.status().c_message();

  uint32_t rows = 1;
  while (res->stmt.Step()) {
    ++rows;
  }
  ASSERT_TRUE(res->stmt.status().ok()) << res->stmt.status().c_message();
  EXPECT_EQ(rows, 4u);
}

// Finalization retires the TEMP table; the next execution drops it safely.
TEST_F(PerfettoSqlConnectionPipelineTest, CompletedPipelineTablesAreDropped) {
  ASSERT_TRUE(Rows("FROM tree").ok());
  auto tables = Rows(
      "SELECT name FROM sqlite_temp_schema WHERE name GLOB "
      "'__intrinsic_pipeline_*'");
  ASSERT_TRUE(tables.ok()) << tables.status().message();
  EXPECT_TRUE(tables->empty());
  auto modules = Rows(
      "SELECT name FROM pragma_module_list WHERE name GLOB "
      "'__intrinsic_pipeline*'");
  ASSERT_TRUE(modules.ok()) << modules.status().message();
  EXPECT_THAT(*modules, testing::ElementsAre("__intrinsic_pipeline"));
}

TEST_F(PerfettoSqlConnectionPipelineTest,
       DifferentSchemasAndConcurrentStatements) {
  auto first =
      connection_->ExecuteUntilLastStatement(SqlSource::FromExecuteQuery(
          "FROM tree |> TREE ACCUMULATE UP SUM(self) AS total"));
  ASSERT_TRUE(first.ok()) << first.status().message();
  auto second = connection_->ExecuteUntilLastStatement(
      SqlSource::FromExecuteQuery("FROM (SELECT name FROM tree)"));
  ASSERT_TRUE(second.ok()) << second.status().message();
  EXPECT_EQ(first->stats.column_count, 5u);
  EXPECT_EQ(second->stats.column_count, 1u);
  uint32_t rows = 1;
  while (first->stmt.Step())
    ++rows;
  EXPECT_EQ(rows, 4u);
  EXPECT_TRUE(first->stmt.status().ok());
  rows = 1;
  while (second->stmt.Step())
    ++rows;
  EXPECT_EQ(rows, 4u);
  EXPECT_TRUE(second->stmt.status().ok());
  // The schema changed after preparing the first statement. Resetting and
  // rerunning it must retain the bound plan through SQLite's reprepare path.
  ASSERT_EQ(sqlite3_reset(first->stmt.sqlite_stmt()), SQLITE_OK);
  rows = 0;
  while (first->stmt.Step())
    ++rows;
  EXPECT_EQ(rows, 4u);
  EXPECT_TRUE(first->stmt.status().ok());
}

TEST_F(PerfettoSqlConnectionPipelineTest,
       CleanupRetriesWhileAnotherStatementIsActive) {
  {
    auto active = connection_->ExecuteUntilLastStatement(
        SqlSource::FromExecuteQuery("FROM tree"));
    ASSERT_TRUE(active.ok()) << active.status().message();
    ASSERT_TRUE(Rows("FROM (SELECT 123 AS value)").ok());
    // The finished pipeline can be retired even though the active statement
    // prevents schema changes. Retrying cleanup must not interrupt either.
    ASSERT_TRUE(Rows("SELECT 1").ok());
    while (active->stmt.Step()) {
    }
    EXPECT_TRUE(active->stmt.status().ok());
  }
  auto tables = Rows(
      "SELECT name FROM sqlite_temp_schema WHERE name GLOB "
      "'__intrinsic_pipeline_*'");
  ASSERT_TRUE(tables.ok()) << tables.status().message();
  EXPECT_TRUE(tables->empty());
}

TEST_F(PerfettoSqlConnectionPipelineTest,
       CleanupAcrossRollbackAndExecutionFailure) {
  ASSERT_TRUE(Rows("FROM tree").ok());
  ASSERT_TRUE(Rows("BEGIN; FROM tree").ok());
  ASSERT_TRUE(Rows("ROLLBACK").ok());
  EXPECT_FALSE(Rows("FROM (SELECT 0 AS id, NULL AS parent_id, 'bad' AS value) "
                    "|> TREE ACCUMULATE UP SUM(value) AS total")
                   .ok());
  auto tables = Rows(
      "SELECT name FROM sqlite_temp_schema WHERE name GLOB "
      "'__intrinsic_pipeline_*'");
  ASSERT_TRUE(tables.ok()) << tables.status().message();
  EXPECT_TRUE(tables->empty());
}

TEST_F(PerfettoSqlConnectionPipelineTest,
       FailedCreateDoesNotRetireAnExistingTable) {
  ASSERT_TRUE(Rows("CREATE TEMP TABLE __intrinsic_pipeline_0(value); "
                   "INSERT INTO __intrinsic_pipeline_0 VALUES(123)")
                  .ok());
  EXPECT_FALSE(Rows("FROM tree").ok());
  auto existing = Rows("SELECT value FROM temp.__intrinsic_pipeline_0");
  ASSERT_TRUE(existing.ok()) << existing.status().message();
  EXPECT_THAT(*existing, testing::ElementsAre("123"));
}

TEST_F(PerfettoSqlConnectionPipelineTest, TemporaryTablesAreConnectionLocal) {
  auto fork = connection_->Fork();
  auto first = connection_->ExecuteUntilLastStatement(
      SqlSource::FromExecuteQuery("FROM (SELECT 123 AS value)"));
  auto second = fork->ExecuteUntilLastStatement(SqlSource::FromExecuteQuery(
      "FROM (SELECT 456 AS value, 'fork' AS name)"));
  ASSERT_TRUE(first.ok()) << first.status().message();
  ASSERT_TRUE(second.ok()) << second.status().message();
  EXPECT_EQ(sqlite3_column_int(first->stmt.sqlite_stmt(), 0), 123);
  EXPECT_EQ(sqlite3_column_int(second->stmt.sqlite_stmt(), 0), 456);
  EXPECT_EQ(first->stats.column_count, 1u);
  EXPECT_EQ(second->stats.column_count, 2u);
}

TEST_F(PerfettoSqlConnectionPipelineTest,
       OutputConstraintsApplyAfterAccumulation) {
  auto context = std::make_unique<PipelineModule::Context>();
  context->pool = &pool_;
  auto* ctx = context.get();
  connection_->RegisterVirtualTableModule<PipelineModule>("test_pipeline",
                                                          std::move(context));
  ASSERT_TRUE(
      Rows("CREATE VIRTUAL TABLE temp.test_output USING test_pipeline(4)")
          .ok());

  pipeline::LogicalPlan logical;
  for (const char* name : {"id", "parent_id", "self"}) {
    auto id = logical.AddColumn(name, core::Int64{});
    logical.output.push_back({name, id});
  }
  logical.ops.emplace_back(pipeline::op::Scan{
      SqlSource::FromExecuteQuery("SELECT id, parent_id, self FROM tree"),
      logical.output});
  auto total = logical.AddColumn("total", core::Int64{});
  pipeline::op::TreeAccumulate fold;
  fold.direction = pipeline::op::TreeDirection::kUp;
  fold.node_column = 0;
  fold.parent_column = 1;
  fold.aggregates.push_back(
      {pipeline::op::TreeAccumulate::Function::kSum, 2, total});
  logical.ops.emplace_back(std::move(fold));
  logical.output.push_back({"total", total});
  pipeline::LowerEnvironment env{connection_->sqlite_connection(), &pool_};
  // The root is last in child-first output. Filtering by its output rowid
  // must retain all descendants while calculating its total.
  for (const char* rhs : {"3", "3.0", "'3'"}) {
    auto stmt = connection_->sqlite_connection()->PrepareStatement(
        SqlSource::FromExecuteQuery(
            "SELECT c3 FROM temp.test_output(?) WHERE rowid = " +
            std::string(rhs) + " AND c3 > 50"));
    ASSERT_TRUE(stmt.status().ok()) << stmt.status().message();
    PipelineModule::Invocation invocation{ctx, "unused",
                                          pipeline::Lower(logical, env)};
    ASSERT_EQ(sqlite3_bind_pointer(stmt.sqlite_stmt(), 1, &invocation,
                                   PipelineModule::kPlanPointerType, nullptr),
              SQLITE_OK);
    ASSERT_TRUE(stmt.Step()) << stmt.status().message();
    EXPECT_EQ(sqlite3_column_int64(stmt.sqlite_stmt(), 0), 100);
    EXPECT_FALSE(stmt.Step());
    EXPECT_TRUE(stmt.status().ok());
    // Explicitly close cursors before the borrowed plan goes out of scope.
    sqlite3_reset(stmt.sqlite_stmt());
  }
}

TEST_F(PerfettoSqlConnectionPipelineTest, ColumnReadersRefreshAcrossBatches) {
  ASSERT_TRUE(connection_
                  ->Execute(SqlSource::FromExecuteQuery(R"(
    CREATE PERFETTO TABLE values_table AS
    WITH RECURSIVE numbers(n) AS (SELECT 0 UNION ALL SELECT n + 1 FROM numbers WHERE n < 4999)
    SELECT n AS id, n - 5000 AS negative, n + 10000000000 AS large,
           n + 0.5 AS real_value, CASE WHEN n % 2 = 0 THEN n END AS nullable,
           CASE WHEN n % 3 = 0 THEN 'text' END AS text_value
    FROM numbers;
  )"))
                  .ok());
  auto res = connection_->ExecuteUntilLastStatement(
      SqlSource::FromExecuteQuery("FROM values_table"));
  ASSERT_TRUE(res.ok()) << res.status().message();
  uint32_t count = 0;
  for (bool more = !res->stmt.IsDone(); more; more = res->stmt.Step()) {
    sqlite3_stmt* stmt = res->stmt.sqlite_stmt();
    EXPECT_EQ(sqlite3_column_int64(stmt, 0), static_cast<int64_t>(count));
    EXPECT_EQ(sqlite3_column_int64(stmt, 1),
              static_cast<int64_t>(count) - 5000);
    EXPECT_EQ(sqlite3_column_int64(stmt, 2),
              static_cast<int64_t>(count) + 10000000000LL);
    EXPECT_EQ(sqlite3_column_double(stmt, 3), count + 0.5);
    if (count % 2 == 0)
      EXPECT_EQ(sqlite3_column_int64(stmt, 4), static_cast<int64_t>(count));
    else
      EXPECT_EQ(sqlite3_column_type(stmt, 4), SQLITE_NULL);
    if (count % 3 == 0)
      EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5)),
                   "text");
    else
      EXPECT_EQ(sqlite3_column_type(stmt, 5), SQLITE_NULL);
    ++count;
  }
  EXPECT_TRUE(res->stmt.status().ok());
  EXPECT_EQ(count, 5000u);
  // A dynamically typed column must continue dispatching per value, including
  // a type change in a later batch.
  auto mixed = Rows(
      "FROM (SELECT CASE WHEN id < 3000 THEN id ELSE 'last' END AS value FROM "
      "values_table)");
  ASSERT_TRUE(mixed.ok()) << mixed.status().message();
  EXPECT_EQ(mixed->size(), 5000u);
  EXPECT_EQ(std::count(mixed->begin(), mixed->end(), "last"), 2000);
}

}  // namespace
}  // namespace perfetto::trace_processor
