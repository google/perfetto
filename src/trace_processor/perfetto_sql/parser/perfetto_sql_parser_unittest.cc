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

#include "src/trace_processor/perfetto_sql/parser/perfetto_sql_parser.h"

#include <string>
#include <utility>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/perfetto_sql/parser/function_util.h"
#include "src/trace_processor/perfetto_sql/parser/perfetto_sql_test_utils.h"
#include "src/trace_processor/perfetto_sql/preprocessor/perfetto_sql_preprocessor.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/util/sql_argument.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {

using Result = PerfettoSqlParser::Statement;
using Statement = PerfettoSqlParser::Statement;
using SqliteSql = PerfettoSqlParser::SqliteSql;
using CreateFn = PerfettoSqlParser::CreateFunction;
using CreateTable = PerfettoSqlParser::CreateTable;
using CreateView = PerfettoSqlParser::CreateView;
using Include = PerfettoSqlParser::Include;
using CreateMacro = PerfettoSqlParser::CreateMacro;
using CreateIndex = PerfettoSqlParser::CreateIndex;

namespace {

class PerfettoSqlParserTest : public ::testing::Test {
 protected:
  base::StatusOr<std::vector<PerfettoSqlParser::Statement>> Parse(
      SqlSource sql) {
    PerfettoSqlParser parser(std::move(sql), macros_);
    std::vector<PerfettoSqlParser::Statement> results;
    while (parser.Next()) {
      results.push_back(parser.statement());
    }
    if (!parser.status().ok()) {
      return parser.status();
    }
    return results;
  }

  base::FlatHashMap<std::string, PerfettoSqlPreprocessor::Macro> macros_;
};

TEST_F(PerfettoSqlParserTest, Empty) {
  ASSERT_THAT(*Parse(SqlSource::FromExecuteQuery("")), testing::IsEmpty());
}

TEST_F(PerfettoSqlParserTest, SemiColonTerminatedStatement) {
  SqlSource res = SqlSource::FromExecuteQuery("SELECT * FROM slice;");
  PerfettoSqlParser parser(res, macros_);
  ASSERT_TRUE(parser.Next());
  ASSERT_EQ(parser.statement(), Statement{SqliteSql{}});
  ASSERT_EQ(parser.statement_sql(), FindSubstr(res, "SELECT * FROM slice"));
}

TEST_F(PerfettoSqlParserTest, MultipleStmts) {
  auto res =
      SqlSource::FromExecuteQuery("SELECT * FROM slice; SELECT * FROM s");
  PerfettoSqlParser parser(res, macros_);
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
  PerfettoSqlParser parser(res, macros_);
  ASSERT_TRUE(parser.Next());
  ASSERT_EQ(parser.statement(), Statement{SqliteSql{}});
  ASSERT_EQ(parser.statement_sql().sql(),
            FindSubstr(res, "SELECT * FROM s").sql());
}

TEST_F(PerfettoSqlParserTest, CreatePerfettoFunctionScalar) {
  auto res = SqlSource::FromExecuteQuery(
      "create perfetto function foo() returns INT as select 1");
  ASSERT_THAT(*Parse(res), testing::ElementsAre(CreateFn{
                               false,
                               FunctionPrototype{"foo", {}},
                               CreateFn::Returns{
                                   false,
                                   sql_argument::Type::kLong,
                                   {},
                               },
                               FindSubstr(res, "select 1"),
                               "",
                               std::nullopt,
                           }));

  res = SqlSource::FromExecuteQuery(
      "create perfetto function bar(x INT, y LONG) returns STRING as "
      "select 'foo'");
  ASSERT_THAT(*Parse(res), testing::ElementsAre(CreateFn{
                               false,
                               FunctionPrototype{
                                   "bar",
                                   {
                                       {"$x", sql_argument::Type::kLong},
                                       {"$y", sql_argument::Type::kLong},
                                   },
                               },
                               CreateFn::Returns{
                                   false,
                                   sql_argument::Type::kString,
                                   {},
                               },
                               FindSubstr(res, "select 'foo'"),
                               "",
                               std::nullopt,
                           }));

  res = SqlSource::FromExecuteQuery(
      "CREATE perfetto FuNcTiOn bar(x INT, y LONG) returnS STRING As "
      "select 'foo'");
  ASSERT_THAT(*Parse(res), testing::ElementsAre(CreateFn{
                               false,
                               FunctionPrototype{
                                   "bar",
                                   {
                                       {"$x", sql_argument::Type::kLong},
                                       {"$y", sql_argument::Type::kLong},
                                   },
                               },
                               CreateFn::Returns{
                                   false,
                                   sql_argument::Type::kString,
                                   {},
                               },
                               FindSubstr(res, "select 'foo'"),
                               "",
                               std::nullopt,
                           }));
}

TEST_F(PerfettoSqlParserTest, CreateOrReplacePerfettoFunctionScalar) {
  auto res = SqlSource::FromExecuteQuery(
      "create or replace perfetto function foo() returns INT as select 1");
  ASSERT_THAT(*Parse(res), testing::ElementsAre(CreateFn{
                               true,
                               FunctionPrototype{"foo", {}},
                               CreateFn::Returns{
                                   false,
                                   sql_argument::Type::kLong,
                                   {},
                               },
                               FindSubstr(res, "select 1"),
                               "",
                               std::nullopt,
                           }));
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
  PerfettoSqlParser parser(res, macros_);
  ASSERT_TRUE(parser.Next());
  CreateFn fn{
      false,
      FunctionPrototype{"foo", {}},
      CreateFn::Returns{
          false,
          sql_argument::Type::kLong,
          {},
      },
      FindSubstr(res, "select 1"),
      "",
      std::nullopt,
  };
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

TEST_F(PerfettoSqlParserTest, CreatePerfettoFunctionIntrinsic) {
  auto res = SqlSource::FromExecuteQuery(
      "create perfetto function my_func() returns INT delegates to "
      "my_intrinsic");
  auto parsed = Parse(res);
  ASSERT_TRUE(parsed.status().ok()) << parsed.status().message();
  ASSERT_EQ(parsed->size(), 1u);
  auto& stmt = (*parsed)[0];
  ASSERT_TRUE(std::holds_alternative<CreateFn>(stmt));
  auto& create_fn = std::get<CreateFn>(stmt);
  EXPECT_FALSE(create_fn.replace);
  EXPECT_EQ(create_fn.prototype.function_name, "my_func");
  EXPECT_TRUE(create_fn.target_function.has_value());
  EXPECT_EQ(create_fn.target_function.value(), "my_intrinsic");
}

TEST_F(PerfettoSqlParserTest, CreateOrReplacePerfettoFunctionIntrinsic) {
  auto res = SqlSource::FromExecuteQuery(
      "create or replace perfetto function test() returns INT delegates to "
      "test_intrinsic");
  ASSERT_THAT(*Parse(res),
              testing::ElementsAre(CreateFn{
                  true,
                  FunctionPrototype{"test", {}},
                  CreateFn::Returns{
                      false,
                      sql_argument::Type::kLong,
                      {},
                  },
                  SqlSource::FromTraceProcessorImplementation(""),
                  "",
                  std::make_optional(std::string("test_intrinsic")),
              }));
}

TEST_F(PerfettoSqlParserTest, CreatePerfettoFunctionIntrinsicError) {
  // Test missing intrinsic name
  auto res = SqlSource::FromExecuteQuery(
      "create perfetto function foo() returns INT delegates to");
  ASSERT_FALSE(Parse(res).status().ok());
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

TEST_F(PerfettoSqlParserTest, CreatePerfettoMacro) {
  auto res = SqlSource::FromExecuteQuery(
      "create perfetto macro foo(a1 Expr, b1 TableOrSubquery,c3_d "
      "TableOrSubquery2 ) returns TableOrSubquery3 as random sql snippet");
  PerfettoSqlParser parser(res, macros_);
  ASSERT_TRUE(parser.Next());
  ASSERT_EQ(
      parser.statement(),
      Statement(CreateMacro{
          false,
          FindSubstr(res, "foo"),
          {
              {FindSubstr(res, "a1"), FindSubstr(res, "Expr")},
              {FindSubstr(res, "b1"), FindSubstr(res, "TableOrSubquery")},
              {FindSubstr(res, "c3_d"), FindSubstr(res, "TableOrSubquery2")},
          },
          FindSubstr(res, "TableOrSubquery3"),
          FindSubstr(res, "random sql snippet")}));
  ASSERT_FALSE(parser.Next());
}

TEST_F(PerfettoSqlParserTest, CreateOrReplacePerfettoMacro) {
  auto res = SqlSource::FromExecuteQuery(
      "create or replace perfetto macro foo() returns Expr as 1");
  PerfettoSqlParser parser(res, macros_);
  ASSERT_TRUE(parser.Next());
  ASSERT_EQ(parser.statement(), Statement(CreateMacro{true,
                                                      FindSubstr(res, "foo"),
                                                      {},
                                                      FindSubstr(res, "Expr"),
                                                      FindSubstr(res, "1")}));
  ASSERT_FALSE(parser.Next());
}

TEST_F(PerfettoSqlParserTest, CreatePerfettoMacroAndOther) {
  auto res = SqlSource::FromExecuteQuery(
      "create perfetto macro foo() returns sql1 as random sql snippet; "
      "select 1");
  PerfettoSqlParser parser(res, macros_);
  ASSERT_TRUE(parser.Next());
  ASSERT_EQ(parser.statement(), Statement(CreateMacro{
                                    false,
                                    FindSubstr(res, "foo"),
                                    {},
                                    FindSubstr(res, "sql1"),
                                    FindSubstr(res, "random sql snippet"),
                                }));
  ASSERT_TRUE(parser.Next());
  ASSERT_EQ(parser.statement(), Statement(SqliteSql{}));
  ASSERT_EQ(parser.statement_sql(), FindSubstr(res, "select 1"));
  ASSERT_FALSE(parser.Next());
}

TEST_F(PerfettoSqlParserTest, CreatePerfettoTable) {
  auto res = SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo AS SELECT 42 AS bar");
  PerfettoSqlParser parser(res, macros_);
  ASSERT_TRUE(parser.Next());
  ASSERT_EQ(parser.statement(), Statement(CreateTable{
                                    false,
                                    "foo",
                                    {},
                                    FindSubstr(res, "SELECT 42 AS bar"),
                                }));
  ASSERT_FALSE(parser.Next());
}

TEST_F(PerfettoSqlParserTest, CreateOrReplacePerfettoTable) {
  auto res = SqlSource::FromExecuteQuery(
      "CREATE OR REPLACE PERFETTO TABLE foo AS SELECT 42 AS bar");
  PerfettoSqlParser parser(res, macros_);
  ASSERT_TRUE(parser.Next());
  ASSERT_EQ(parser.statement(), Statement(CreateTable{
                                    true,
                                    "foo",
                                    {},
                                    FindSubstr(res, "SELECT 42 AS bar"),
                                }));
  ASSERT_FALSE(parser.Next());
}

TEST_F(PerfettoSqlParserTest, CreatePerfettoTableWithSchema) {
  auto res = SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo(bar INT) AS SELECT 42 AS bar");
  PerfettoSqlParser parser(res, macros_);
  ASSERT_TRUE(parser.Next());
  ASSERT_EQ(parser.statement(), Statement(CreateTable{
                                    false,
                                    "foo",
                                    {{"$bar", sql_argument::Type::kLong}},
                                    FindSubstr(res, "SELECT 42 AS bar"),
                                }));
  ASSERT_FALSE(parser.Next());
}

TEST_F(PerfettoSqlParserTest, CreatePerfettoTableAndOther) {
  auto res = SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo AS SELECT 42 AS bar; select 1");
  PerfettoSqlParser parser(res, macros_);
  ASSERT_TRUE(parser.Next());
  ASSERT_EQ(parser.statement(),
            Statement(CreateTable{
                false, "foo", {}, FindSubstr(res, "SELECT 42 AS bar")}));
  ASSERT_TRUE(parser.Next());
  ASSERT_EQ(parser.statement(), Statement(SqliteSql{}));
  ASSERT_EQ(parser.statement_sql(), FindSubstr(res, "select 1"));
  ASSERT_FALSE(parser.Next());
}

TEST_F(PerfettoSqlParserTest, CreatePerfettoTableWithDataframe) {
  auto res = SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo USING DATAFRAME AS SELECT 42 AS bar");
  PerfettoSqlParser parser(res, macros_);
  ASSERT_TRUE(parser.Next());
  ASSERT_EQ(parser.statement(),
            Statement(CreateTable{
                false, "foo", {}, FindSubstr(res, "SELECT 42 AS bar")}));
  ASSERT_FALSE(parser.Next());
}

TEST_F(PerfettoSqlParserTest, CreatePerfettoView) {
  auto res = SqlSource::FromExecuteQuery(
      "CREATE PERFETTO VIEW foo AS SELECT 42 AS bar");
  PerfettoSqlParser parser(res, macros_);
  ASSERT_TRUE(parser.Next());
  ASSERT_EQ(
      parser.statement(),
      Statement(CreateView{
          false,
          "foo",
          {},
          SqlSource::FromExecuteQuery("SELECT 42 AS bar"),
          SqlSource::FromExecuteQuery("CREATE VIEW foo AS SELECT 42 AS bar"),
      }));
  ASSERT_FALSE(parser.Next());
}

TEST_F(PerfettoSqlParserTest, CreateOrReplacePerfettoView) {
  auto res = SqlSource::FromExecuteQuery(
      "CREATE OR REPLACE PERFETTO VIEW foo AS SELECT 42 AS bar");
  PerfettoSqlParser parser(res, macros_);
  ASSERT_TRUE(parser.Next());
  ASSERT_EQ(
      parser.statement(),
      Statement(CreateView{
          true,
          "foo",
          {},
          SqlSource::FromExecuteQuery("SELECT 42 AS bar"),
          SqlSource::FromExecuteQuery("CREATE VIEW foo AS SELECT 42 AS bar"),
      }));
  ASSERT_FALSE(parser.Next());
}

TEST_F(PerfettoSqlParserTest, CreatePerfettoViewAndOther) {
  auto res = SqlSource::FromExecuteQuery(
      "CREATE PERFETTO VIEW foo AS SELECT 42 AS bar; select 1");
  PerfettoSqlParser parser(res, macros_);
  ASSERT_TRUE(parser.Next());
  ASSERT_EQ(
      parser.statement(),
      Statement(CreateView{
          false,
          "foo",
          {},
          SqlSource::FromExecuteQuery("SELECT 42 AS bar"),
          SqlSource::FromExecuteQuery("CREATE VIEW foo AS SELECT 42 AS bar"),
      }));
  ASSERT_TRUE(parser.Next());
  ASSERT_EQ(parser.statement(), Statement(SqliteSql{}));
  ASSERT_EQ(parser.statement_sql(), FindSubstr(res, "select 1"));
  ASSERT_FALSE(parser.Next());
}

TEST_F(PerfettoSqlParserTest, CreatePerfettoViewWithSchema) {
  auto res = SqlSource::FromExecuteQuery(
      "CREATE PERFETTO VIEW foo(foo STRING, bar INT) AS SELECT 'a' as foo, 42 "
      "AS bar");
  PerfettoSqlParser parser(res, macros_);
  ASSERT_TRUE(parser.Next());
  ASSERT_EQ(parser.statement(),
            Statement(CreateView{
                false,
                "foo",
                {
                    {"$foo", sql_argument::Type::kString},
                    {"$bar", sql_argument::Type::kLong},
                },
                SqlSource::FromExecuteQuery("SELECT 'a' as foo, 42 AS bar"),
                SqlSource::FromExecuteQuery(
                    "CREATE VIEW foo AS SELECT 'a' as foo, 42 AS bar"),
            }));
  ASSERT_FALSE(parser.Next());
}

TEST_F(PerfettoSqlParserTest, ParseComplexArgumentType) {
  auto res = SqlSource::FromExecuteQuery(
      "CREATE PERFETTO VIEW foo(foo JOINID(foo.bar), bar LONG) AS SELECT "
      "'a' as foo, 42 "
      "AS bar");
  PerfettoSqlParser parser(res, macros_);
  ASSERT_TRUE(parser.Next());
  ASSERT_EQ(parser.statement(),
            Statement(CreateView{
                false,
                "foo",
                {
                    {"$foo", sql_argument::Type::kLong},
                    {"$bar", sql_argument::Type::kLong},
                },
                SqlSource::FromExecuteQuery("SELECT 'a' as foo, 42 AS bar"),
                SqlSource::FromExecuteQuery(
                    "CREATE VIEW foo AS SELECT 'a' as foo, 42 AS bar"),
            }));
  ASSERT_FALSE(parser.Next());
}

}  // namespace
}  // namespace perfetto::trace_processor
