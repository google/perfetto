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

#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_parser.h"

#include <cstdint>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_test_utils.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {

using Result = PerfettoSqlParser::Statement;
using Statement = PerfettoSqlParser::Statement;
using SqliteSql = PerfettoSqlParser::SqliteSql;
using CreateFn = PerfettoSqlParser::CreateFunction;
using CreateTable = PerfettoSqlParser::CreateTable;
using Include = PerfettoSqlParser::Include;

namespace {

class PerfettoSqlParserTest : public ::testing::Test {
 protected:
  base::StatusOr<std::vector<PerfettoSqlParser::Statement>> Parse(
      SqlSource sql) {
    PerfettoSqlParser parser(sql);
    std::vector<PerfettoSqlParser::Statement> results;
    while (parser.Next()) {
      results.push_back(std::move(parser.statement()));
    }
    if (!parser.status().ok()) {
      return parser.status();
    }
    return results;
  }
};

TEST_F(PerfettoSqlParserTest, Empty) {
  ASSERT_THAT(*Parse(SqlSource::FromExecuteQuery("")), testing::IsEmpty());
}

TEST_F(PerfettoSqlParserTest, SemiColonTerminatedStatement) {
  SqlSource res = SqlSource::FromExecuteQuery("SELECT * FROM slice;");
  PerfettoSqlParser parser(res);
  ASSERT_TRUE(parser.Next());
  ASSERT_EQ(parser.statement(), Statement{SqliteSql{}});
  ASSERT_EQ(parser.statement_sql(), FindSubstr(res, "SELECT * FROM slice"));
}

TEST_F(PerfettoSqlParserTest, MultipleStmts) {
  auto res =
      SqlSource::FromExecuteQuery("SELECT * FROM slice; SELECT * FROM s");
  PerfettoSqlParser parser(res);
  ASSERT_TRUE(parser.Next());
  ASSERT_EQ(parser.statement(), Statement{SqliteSql{}});
  ASSERT_EQ(parser.statement_sql().sql(),
            FindSubstr(res, "SELECT * FROM slice").sql());
  ASSERT_TRUE(parser.Next());
  ASSERT_EQ(parser.statement(), Statement{SqliteSql{}});
  ASSERT_EQ(parser.statement_sql().sql(),
            FindSubstr(res, "SELECT * FROM s").sql());
}

TEST_F(PerfettoSqlParserTest, IgnoreOnlySpace) {
  auto res = SqlSource::FromExecuteQuery(" ; SELECT * FROM s; ; ;");
  PerfettoSqlParser parser(res);
  ASSERT_TRUE(parser.Next());
  ASSERT_EQ(parser.statement(), Statement{SqliteSql{}});
  ASSERT_EQ(parser.statement_sql().sql(),
            FindSubstr(res, "SELECT * FROM s").sql());
}

TEST_F(PerfettoSqlParserTest, CreatePerfettoFunctionScalar) {
  auto res = SqlSource::FromExecuteQuery(
      "create perfetto function foo() returns INT as select 1");
  ASSERT_THAT(*Parse(res),
              testing::ElementsAre(CreateFn{
                  false, "foo()", "INT", FindSubstr(res, "select 1"), false}));

  res = SqlSource::FromExecuteQuery(
      "create perfetto function bar(x INT, y LONG) returns STRING as "
      "select 'foo'");
  ASSERT_THAT(*Parse(res), testing::ElementsAre(CreateFn{
                               false, "bar(x INT, y LONG)", "STRING",
                               FindSubstr(res, "select 'foo'"), false}));

  res = SqlSource::FromExecuteQuery(
      "CREATE perfetto FuNcTiOn bar(x INT, y LONG) returnS STRING As "
      "select 'foo'");
  ASSERT_THAT(*Parse(res), testing::ElementsAre(CreateFn{
                               false, "bar(x INT, y LONG)", "STRING",
                               FindSubstr(res, "select 'foo'"), false}));
}

TEST_F(PerfettoSqlParserTest, CreatePerfettoFunctionScalarError) {
  auto res = SqlSource::FromExecuteQuery(
      "create perfetto function foo( returns INT as select 1");
  ASSERT_FALSE(Parse(res).status().ok());

  res = SqlSource::FromExecuteQuery(
      "create perfetto function foo(x INT) as select 1");
  ASSERT_FALSE(Parse(res).status().ok());

  res = SqlSource::FromExecuteQuery(
      "create perfetto function foo(x INT) returns INT");
  ASSERT_FALSE(Parse(res).status().ok());
}

TEST_F(PerfettoSqlParserTest, CreatePerfettoFunctionAndOther) {
  auto res = SqlSource::FromExecuteQuery(
      "create perfetto function foo() returns INT as select 1; select foo()");
  PerfettoSqlParser parser(res);
  ASSERT_TRUE(parser.Next());
  CreateFn fn{false, "foo()", "INT", FindSubstr(res, "select 1"), false};
  ASSERT_EQ(parser.statement(), Statement{fn});
  ASSERT_EQ(
      parser.statement_sql().sql(),
      FindSubstr(res, "create perfetto function foo() returns INT as select 1")
          .sql());
  ASSERT_TRUE(parser.Next());
  ASSERT_EQ(parser.statement(), Statement{SqliteSql{}});
  ASSERT_EQ(parser.statement_sql().sql(),
            FindSubstr(res, "select foo()").sql());
}

TEST_F(PerfettoSqlParserTest, IncludePerfettoTrivial) {
  auto res =
      SqlSource::FromExecuteQuery("include perfetto module cheese.bre_ad;");
  ASSERT_THAT(*Parse(res), testing::ElementsAre(Include{"cheese.bre_ad"}));
}

TEST_F(PerfettoSqlParserTest, IncludePerfettoErrorAdditionalChars) {
  auto res = SqlSource::FromExecuteQuery(
      "include perfetto module cheese.bre_ad blabla;");
  ASSERT_FALSE(Parse(res).status().ok());
}

TEST_F(PerfettoSqlParserTest, IncludePerfettoErrorWrongModuleName) {
  auto res =
      SqlSource::FromExecuteQuery("include perfetto module chees*e.bre_ad;");
  ASSERT_FALSE(Parse(res).status().ok());
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
