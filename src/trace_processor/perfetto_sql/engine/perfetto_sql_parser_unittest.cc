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
#include "src/trace_processor/sqlite/sql_source.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {

using Result = PerfettoSqlParser::Statement;
using SqliteSql = PerfettoSqlParser::SqliteSql;
using CreateFn = PerfettoSqlParser::CreateFunction;
using CreateTable = PerfettoSqlParser::CreateTable;

inline bool operator==(const SqlSource& a, const SqlSource& b) {
  return a.sql() == b.sql();
}

inline bool operator==(const SqliteSql& a, const SqliteSql& b) {
  return a.sql == b.sql;
}

inline bool operator==(const CreateFn& a, const CreateFn& b) {
  return std::tie(a.returns, a.is_table, a.prototype, a.replace, a.sql) ==
         std::tie(b.returns, b.is_table, b.prototype, b.replace, b.sql);
}

inline bool operator==(const CreateTable& a, const CreateTable& b) {
  return std::tie(a.name, a.sql) == std::tie(b.name, b.sql);
}

namespace {

SqlSource FindSubstr(const SqlSource& source, const std::string& needle) {
  size_t off = source.sql().find(needle);
  PERFETTO_CHECK(off != std::string::npos);
  return source.Substr(static_cast<uint32_t>(off),
                       static_cast<uint32_t>(needle.size()));
}

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
  auto res = SqlSource::FromExecuteQuery("SELECT * FROM slice;");
  ASSERT_THAT(*Parse(res), testing::ElementsAre(SqliteSql{res}));
}

TEST_F(PerfettoSqlParserTest, MultipleStmts) {
  auto res =
      SqlSource::FromExecuteQuery("SELECT * FROM slice; SELECT * FROM s");
  ASSERT_THAT(*Parse(res), testing::ElementsAre(SqliteSql{res.Substr(0, 20)},
                                                SqliteSql{res.Substr(21, 15)}));
}

TEST_F(PerfettoSqlParserTest, IgnoreOnlySpace) {
  auto res = SqlSource::FromExecuteQuery(" ; SELECT * FROM s; ; ;");
  ASSERT_THAT(*Parse(res), testing::ElementsAre(SqliteSql{res.Substr(3, 16)}));
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
  ASSERT_THAT(*Parse(res), testing::ElementsAre(
                               CreateFn{false, "foo()", "INT",
                                        FindSubstr(res, "select 1;"), false},
                               SqliteSql{FindSubstr(res, "select foo()")}));
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
