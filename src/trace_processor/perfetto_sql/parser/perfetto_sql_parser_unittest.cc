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

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/perfetto_sql/parser/function_util.h"
#include "src/trace_processor/perfetto_sql/parser/perfetto_sql_test_utils.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/test_catalog.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_connection.h"
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
using Pipeline = PerfettoSqlParser::Pipeline;
using testing::HasSubstr;

namespace {

class PerfettoSqlParserTest : public ::testing::Test {
 protected:
  PerfettoSqlParserTest()
      : connection_(SqliteConnection::CreateConnectionToNewDatabase()),
        catalog_(&pool_, connection_.get()) {
    catalog_.AddTable("slice", {"id", "parent_id", "dur", "self", "depth"},
                      {{0, std::nullopt, 10, 10, 0}, {1, 0, 5, 5, 1}});
    auto stmt = connection_->PrepareStatement(SqlSource::FromExecuteQuery(
        "CREATE TABLE tree(id INTEGER, parent_id INTEGER, self INTEGER)"));
    while (stmt.Step()) {
    }
    PERFETTO_CHECK(stmt.status().ok());
  }

  base::StatusOr<std::vector<PerfettoSqlParser::Statement>> Parse(
      SqlSource sql) {
    PerfettoSqlParser parser(macros_, catalog_,
                             /*pipelines_allowed=*/true);
    parser.Reset(std::move(sql));
    std::vector<PerfettoSqlParser::Statement> results;
    while (parser.Next()) {
      results.push_back(parser.statement());
    }
    if (!parser.status().ok()) {
      return parser.status();
    }
    return results;
  }

  // Registers a user-defined macro visible to `Parse()`, as if it had been
  // declared via `CREATE PERFETTO MACRO`.  `param_names` are bare names
  // (no `$` prefix); inside `body`, placeholders are written as `$name`.
  void RegisterMacro(const std::string& name,
                     std::vector<std::string> param_names,
                     const std::string& body) {
    macros_.Insert(name, PerfettoSqlParser::Macro{
                             /*replace=*/false,
                             name,
                             std::move(param_names),
                             SqlSource::FromTraceProcessorImplementation(body),
                         });
  }

  // Parses `sql` as a single statement and returns its `statement_sql()`.
  // The caller is responsible for asserting `sql` is syntactically well-formed
  // and produces exactly one statement; failures abort the test.
  SqlSource ParseOne(SqlSource sql) {
    PerfettoSqlParser parser(macros_, catalog_,
                             /*pipelines_allowed=*/false);
    parser.Reset(std::move(sql));
    PERFETTO_CHECK(parser.Next());
    PERFETTO_CHECK(parser.status().ok());
    SqlSource out = parser.statement_sql();
    PERFETTO_CHECK(!parser.Next());
    return out;
  }

  // Parses a single pipeline statement and returns its plan as text.
  base::StatusOr<std::string> ParsePipeline(const std::string& sql) {
    ASSIGN_OR_RETURN(auto parsed, Parse(SqlSource::FromExecuteQuery(sql)));
    PERFETTO_CHECK(parsed.size() == 1);
    const auto* pipeline = std::get_if<Pipeline>(&parsed[0]);
    PERFETTO_CHECK(pipeline);
    return pipeline::LogicalPlanToString(pipeline->plan);
  }

  base::FlatHashMap<std::string, PerfettoSqlParser::Macro> macros_;
  StringPool pool_;
  std::unique_ptr<SqliteConnection> connection_;
  pipeline::TestCatalog catalog_;
};

TEST_F(PerfettoSqlParserTest, Empty) {
  ASSERT_THAT(*Parse(SqlSource::FromExecuteQuery("")), testing::IsEmpty());
}

TEST_F(PerfettoSqlParserTest, SemiColonTerminatedStatement) {
  SqlSource res = SqlSource::FromExecuteQuery("SELECT * FROM slice;");
  PerfettoSqlParser parser(macros_, catalog_,
                           /*pipelines_allowed=*/false);
  parser.Reset(res);
  ASSERT_TRUE(parser.Next());
  ASSERT_EQ(parser.statement(), Statement{SqliteSql{}});
  ASSERT_EQ(parser.statement_sql(), FindSubstr(res, "SELECT * FROM slice"));
}

TEST_F(PerfettoSqlParserTest, ExplainKeepsPrefix) {
  SqlSource res =
      SqlSource::FromExecuteQuery("EXPLAIN QUERY PLAN SELECT * FROM slice;");
  PerfettoSqlParser parser(macros_, catalog_,
                           /*pipelines_allowed=*/false);
  parser.Reset(res);
  ASSERT_TRUE(parser.Next());
  ASSERT_EQ(parser.statement(), Statement{SqliteSql{}});
  ASSERT_EQ(parser.statement_sql().sql(),
            FindSubstr(res, "EXPLAIN QUERY PLAN SELECT * FROM slice").sql());

  SqlSource bytecode = SqlSource::FromExecuteQuery("EXPLAIN SELECT 1");
  ASSERT_EQ(ParseOne(bytecode).sql(),
            FindSubstr(bytecode, "EXPLAIN SELECT 1").sql());
}

TEST_F(PerfettoSqlParserTest, ExplainWithMacro) {
  RegisterMacro("foo", {}, "SELECT 1");
  SqlSource res = SqlSource::FromExecuteQuery("EXPLAIN QUERY PLAN foo!();");
  ASSERT_EQ(ParseOne(res).sql(), "EXPLAIN QUERY PLAN SELECT 1");
}

TEST_F(PerfettoSqlParserTest, MultipleStmts) {
  auto res =
      SqlSource::FromExecuteQuery("SELECT * FROM slice; SELECT * FROM s");
  PerfettoSqlParser parser(macros_, catalog_,
                           /*pipelines_allowed=*/false);
  parser.Reset(res);
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
  PerfettoSqlParser parser(macros_, catalog_,
                           /*pipelines_allowed=*/false);
  parser.Reset(res);
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
  PerfettoSqlParser parser(macros_, catalog_,
                           /*pipelines_allowed=*/false);
  parser.Reset(res);
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

TEST_F(PerfettoSqlParserTest, CreatePerfettoFunctionVariadicDelegate) {
  // Variadic arguments should work in delegate functions
  auto res = SqlSource::FromExecuteQuery(
      "create perfetto function foo(args ANY...) returns INT delegates to "
      "my_intrinsic");
  auto parsed = Parse(res);
  ASSERT_TRUE(parsed.status().ok()) << parsed.status().message();
  ASSERT_EQ(parsed->size(), 1u);
  auto& stmt = (*parsed)[0];
  ASSERT_TRUE(std::holds_alternative<CreateFn>(stmt));
  auto& create_fn = std::get<CreateFn>(stmt);
  EXPECT_EQ(create_fn.prototype.function_name, "foo");
  ASSERT_EQ(create_fn.prototype.arguments.size(), 1u);
  EXPECT_EQ(create_fn.prototype.arguments[0].name().ToStdString(), "args");
  EXPECT_EQ(create_fn.prototype.arguments[0].type(), sql_argument::Type::kAny);
  EXPECT_TRUE(create_fn.prototype.arguments[0].is_variadic());
  EXPECT_TRUE(create_fn.target_function.has_value());
}

TEST_F(PerfettoSqlParserTest, CreatePerfettoFunctionVariadicWithOtherArgs) {
  // Variadic argument can follow non-variadic arguments
  auto res = SqlSource::FromExecuteQuery(
      "create perfetto function foo(x INT, args ANY...) returns INT delegates "
      "to my_intrinsic");
  auto parsed = Parse(res);
  ASSERT_TRUE(parsed.status().ok()) << parsed.status().message();
  ASSERT_EQ(parsed->size(), 1u);
  auto& stmt = (*parsed)[0];
  ASSERT_TRUE(std::holds_alternative<CreateFn>(stmt));
  auto& create_fn = std::get<CreateFn>(stmt);
  ASSERT_EQ(create_fn.prototype.arguments.size(), 2u);
  EXPECT_EQ(create_fn.prototype.arguments[0].name().ToStdString(), "x");
  EXPECT_FALSE(create_fn.prototype.arguments[0].is_variadic());
  EXPECT_EQ(create_fn.prototype.arguments[1].name().ToStdString(), "args");
  EXPECT_TRUE(create_fn.prototype.arguments[1].is_variadic());
}

TEST_F(PerfettoSqlParserTest,
       CreatePerfettoFunctionVariadicInSqlFunctionError) {
  // Variadic arguments should NOT work in SQL functions (with AS body)
  auto res = SqlSource::FromExecuteQuery(
      "create perfetto function foo(args ANY...) returns INT as select 1");
  auto parsed = Parse(res);
  ASSERT_FALSE(parsed.status().ok());
  EXPECT_THAT(parsed.status().message(),
              testing::HasSubstr("Variadic arguments are only allowed in "
                                 "delegate functions"));
}

TEST_F(PerfettoSqlParserTest, CreatePerfettoFunctionVariadicNotLastError) {
  // Variadic argument must be the last argument
  auto res = SqlSource::FromExecuteQuery(
      "create perfetto function foo(args ANY..., x INT) returns INT delegates "
      "to my_intrinsic");
  auto parsed = Parse(res);
  ASSERT_FALSE(parsed.status().ok());
  EXPECT_THAT(parsed.status().message(),
              testing::HasSubstr("Variadic argument must be the last"));
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
  PerfettoSqlParser parser(macros_, catalog_,
                           /*pipelines_allowed=*/false);
  parser.Reset(res);
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
  PerfettoSqlParser parser(macros_, catalog_,
                           /*pipelines_allowed=*/false);
  parser.Reset(res);
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
  PerfettoSqlParser parser(macros_, catalog_,
                           /*pipelines_allowed=*/false);
  parser.Reset(res);
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
  PerfettoSqlParser parser(macros_, catalog_,
                           /*pipelines_allowed=*/false);
  parser.Reset(res);
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
  PerfettoSqlParser parser(macros_, catalog_,
                           /*pipelines_allowed=*/false);
  parser.Reset(res);
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
  PerfettoSqlParser parser(macros_, catalog_,
                           /*pipelines_allowed=*/false);
  parser.Reset(res);
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
  PerfettoSqlParser parser(macros_, catalog_,
                           /*pipelines_allowed=*/false);
  parser.Reset(res);
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
  PerfettoSqlParser parser(macros_, catalog_,
                           /*pipelines_allowed=*/false);
  parser.Reset(res);
  ASSERT_TRUE(parser.Next());
  ASSERT_EQ(parser.statement(),
            Statement(CreateTable{
                false, "foo", {}, FindSubstr(res, "SELECT 42 AS bar")}));
  ASSERT_FALSE(parser.Next());
}

// Columns of the test `slice` table. The builder narrows the types.
constexpr char kSliceColumns[] =
    "#0:id AS id, #1:uint32 AS parent_id, #2:uint32 AS dur, "
    "#3:uint32 AS self, #4:id AS depth";

TEST_F(PerfettoSqlParserTest, Pipeline) {
  auto res = SqlSource::FromExecuteQuery(
      "FROM slice |> TREE ACCUMULATE UP SUM(dur) AS total; SELECT 1");
  PerfettoSqlParser parser(macros_, catalog_,
                           /*pipelines_allowed=*/true);
  parser.Reset(res);
  ASSERT_TRUE(parser.Next());
  const auto* pipeline = std::get_if<Pipeline>(&parser.statement());
  ASSERT_NE(pipeline, nullptr);
  EXPECT_EQ(
      parser.statement_sql(),
      FindSubstr(res, "FROM slice |> TREE ACCUMULATE UP SUM(dur) AS total"));
  EXPECT_EQ(pipeline::LogicalPlanToString(pipeline->plan),
            std::string("Scan(table slice) [") + kSliceColumns + "]\n" +
                "TreeAccumulate(up, node=#0, parent=#1, SUM(#2) -> #5:int64)\n"
                "Output(#0 AS id, #1 AS parent_id, #2 AS dur, #3 AS self, "
                "#4 AS depth, #5 AS total)\n");
  ASSERT_TRUE(parser.Next());
  ASSERT_EQ(parser.statement(), Statement(SqliteSql{}));
  ASSERT_FALSE(parser.Next());
  ASSERT_TRUE(parser.status().ok());
}

TEST_F(PerfettoSqlParserTest, PipelineExpandsMacros) {
  RegisterMacro("stacks", {}, "(SELECT * FROM tree)");
  auto res = SqlSource::FromExecuteQuery(
      "FROM stacks!() |> TREE ACCUMULATE UP SUM(self) AS total");
  PerfettoSqlParser parser(macros_, catalog_,
                           /*pipelines_allowed=*/true);
  parser.Reset(res);
  ASSERT_TRUE(parser.Next()) << parser.status().message();
  const auto* pipeline = std::get_if<Pipeline>(&parser.statement());
  ASSERT_NE(pipeline, nullptr);
  EXPECT_EQ(parser.statement_sql().sql(),
            "FROM (SELECT * FROM tree) |> TREE ACCUMULATE UP SUM(self) AS "
            "total");
  EXPECT_THAT(pipeline::LogicalPlanToString(pipeline->plan),
              HasSubstr("Scan(sql SELECT * FROM (SELECT * FROM tree))"));
}

TEST_F(PerfettoSqlParserTest, CreatePerfettoTableAsPipeline) {
  auto res = SqlSource::FromExecuteQuery(
      "CREATE PERFETTO TABLE foo AS FROM slice |> TREE ACCUMULATE UP "
      "SUM(dur) AS total");
  auto parsed = Parse(res);
  ASSERT_TRUE(parsed.ok()) << parsed.status().message();
  ASSERT_EQ(parsed->size(), 1u);
  const auto* table = std::get_if<CreateTable>(&(*parsed)[0]);
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(table->name, "foo");
  const auto* plan = std::get_if<pipeline::LogicalPlan>(&table->body);
  ASSERT_NE(plan, nullptr);
  EXPECT_EQ(plan->ops.size(), 2u);
}

// `|>` must be `|` immediately followed by `>`.
TEST_F(PerfettoSqlParserTest, PipelineSyntaxErrors) {
  EXPECT_THAT(ParsePipeline("FROM slice | > TREE ACCUMULATE UP SUM(a) AS b")
                  .status()
                  .message(),
              HasSubstr("syntax error"));
  EXPECT_THAT(ParsePipeline("FROM slice |> WHERE dur > 0").status().message(),
              HasSubstr("syntax error near 'WHERE'"));
  EXPECT_THAT(ParsePipeline("FROM a JOIN b ON a.x = b.y |> TREE ACCUMULATE "
                            "UP SUM(a) AS b")
                  .status()
                  .message(),
              HasSubstr("syntax error near 'JOIN'"));
  EXPECT_THAT(ParsePipeline("FROM slice |> TREE ACCUMULATE SUM(a) AS b")
                  .status()
                  .message(),
              HasSubstr("syntax error near 'SUM'"));
  EXPECT_FALSE(ParsePipeline("FROM slice |> TREE ACCUMULATE UP SUM(a)").ok());
}

TEST_F(PerfettoSqlParserTest, PipelineCompileErrors) {
  EXPECT_THAT(ParsePipeline("FROM nope").status().message(),
              HasSubstr("no such table: nope"));
  EXPECT_THAT(ParsePipeline("FROM (SELECT id, self FROM tree) |> TREE "
                            "ACCUMULATE UP SUM(self) AS total")
                  .status()
                  .message(),
              HasSubstr("TREE ACCUMULATE: no such column: 'parent_id'"));
  EXPECT_THAT(ParsePipeline("FROM slice |> TREE ACCUMULATE UP SUM(nope) AS t")
                  .status()
                  .message(),
              HasSubstr("TREE ACCUMULATE: no such column: 'nope'"));
  EXPECT_THAT(ParsePipeline("FROM slice |> TREE ACCUMULATE UP MAX(self) AS t")
                  .status()
                  .message(),
              HasSubstr("aggregate MAX is not supported yet"));
  EXPECT_THAT(ParsePipeline("FROM slice |> TREE ACCUMULATE UP COUNT(*) AS n")
                  .status()
                  .message(),
              HasSubstr("aggregate COUNT is not supported yet"));
  EXPECT_THAT(ParsePipeline("FROM slice |> TREE ACCUMULATE UP SUM(*) AS n")
                  .status()
                  .message(),
              HasSubstr("expected a column name, not *"));
  EXPECT_THAT(ParsePipeline("FROM slice |> TREE ACCUMULATE UP self AS n")
                  .status()
                  .message(),
              HasSubstr("expected an aggregate like SUM(column)"));
  EXPECT_THAT(
      ParsePipeline("FROM slice |> TREE ACCUMULATE UP SUM(self, id) AS n")
          .status()
          .message(),
      HasSubstr("expected exactly one argument"));
  EXPECT_THAT(
      ParsePipeline("FROM slice |> TREE ACCUMULATE UP SUM(DISTINCT self) AS n")
          .status()
          .message(),
      HasSubstr("DISTINCT is not supported yet"));
  EXPECT_THAT(
      ParsePipeline("FROM slice |> TREE ACCUMULATE UP SUM(self + 1) AS n")
          .status()
          .message(),
      HasSubstr("expected a column name"));
}

TEST_F(PerfettoSqlParserTest, PipelineQualifiedColumns) {
  // A qualifier picks the source's column over a later one of the same name.
  auto plan = ParsePipeline(
      "FROM slice |> TREE ACCUMULATE UP SUM(self) AS SELF "
      "|> TREE ACCUMULATE UP SUM(Slice.self) AS total");
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  EXPECT_THAT(*plan, HasSubstr("SUM(#3) -> #6:int64"));

  // An alias renames the qualifier without taking the table off the direct
  // dataframe read.
  plan =
      ParsePipeline("FROM slice AS s |> TREE ACCUMULATE UP SUM(s.self) AS n");
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  EXPECT_THAT(*plan, HasSubstr(std::string("Scan(table slice) [") +
                               kSliceColumns + "]"));
  EXPECT_THAT(ParsePipeline(
                  "FROM slice AS s |> TREE ACCUMULATE UP SUM(slice.self) AS n")
                  .status()
                  .message(),
              HasSubstr("no such column: 'slice.self'"));

  EXPECT_TRUE(
      ParsePipeline("FROM tree AS t |> TREE ACCUMULATE UP SUM(t.self) AS n")
          .ok());
  EXPECT_THAT(
      ParsePipeline("FROM tree AS t |> TREE ACCUMULATE UP SUM(tree.self) AS n")
          .status()
          .message(),
      HasSubstr("no such column: 'tree.self'"));
  EXPECT_THAT(ParsePipeline("FROM (SELECT * FROM tree) |> TREE ACCUMULATE UP "
                            "SUM(tree.self) AS n")
                  .status()
                  .message(),
              HasSubstr("no such column: 'tree.self'"));
  EXPECT_THAT(ParsePipeline(
                  "FROM slice |> TREE ACCUMULATE UP SUM(main.slice.self) AS n")
                  .status()
                  .message(),
              HasSubstr("a schema-qualified column is not supported yet"));
}

TEST_F(PerfettoSqlParserTest, PipelineDuplicateNamesAreAmbiguousOnReference) {
  EXPECT_TRUE(
      ParsePipeline("FROM slice |> TREE ACCUMULATE UP SUM(self) AS SELF").ok());
  EXPECT_TRUE(ParsePipeline("FROM slice |> TREE ACCUMULATE UP SUM(self) AS a, "
                            "SUM(self) AS A")
                  .ok());
  EXPECT_THAT(
      ParsePipeline("FROM slice |> TREE ACCUMULATE UP SUM(self) AS SELF "
                    "|> TREE ACCUMULATE UP SUM(self) AS total")
          .status()
          .message(),
      HasSubstr("column 'self' is ambiguous: it could be `slice.self` or "
                "`TREE ACCUMULATE SUM(self) AS SELF`"));
  EXPECT_THAT(
      ParsePipeline("FROM slice |> TREE ACCUMULATE UP SUM(self) AS a, "
                    "SUM(self) AS A |> TREE ACCUMULATE UP SUM(a) AS total")
          .status()
          .message(),
      HasSubstr("column 'a' is ambiguous: it could be "
                "`TREE ACCUMULATE SUM(self) AS a` or "
                "`TREE ACCUMULATE SUM(self) AS A`"));
}

TEST_F(PerfettoSqlParserTest, PipelineErrorsCarryATraceback) {
  auto plan = ParsePipeline("FROM slice |> TREE ACCUMULATE SIDEWAYS SUM(a)");
  ASSERT_FALSE(plan.ok());
  EXPECT_THAT(plan.status().message(), HasSubstr("^"));

  plan = ParsePipeline("FROM slice\n|> TREE ACCUMULATE UP SUM(nope) AS t");
  ASSERT_FALSE(plan.ok());
  EXPECT_THAT(plan.status().message(), HasSubstr("SUM(nope) AS t\n"));
  EXPECT_THAT(plan.status().message(), HasSubstr("^"));
}

TEST_F(PerfettoSqlParserTest, PipelineAggregatesReadOnlyTheirInput) {
  EXPECT_THAT(ParsePipeline("FROM slice |> TREE ACCUMULATE UP SUM(self) AS a, "
                            "SUM(a) AS b")
                  .status()
                  .message(),
              HasSubstr("no such column: 'a'"));
  EXPECT_TRUE(ParsePipeline("FROM slice |> TREE ACCUMULATE UP SUM(self) AS a "
                            "|> TREE ACCUMULATE UP SUM(a) AS b")
                  .ok());
}

TEST_F(PerfettoSqlParserTest, TakePipelineStatement) {
  PerfettoSqlParser parser(macros_, catalog_,
                           /*pipelines_allowed=*/true);
  parser.Reset(SqlSource::FromExecuteQuery("FROM slice; SELECT 1"));
  ASSERT_TRUE(parser.Next());
  auto statement = parser.TakeStatement();
  EXPECT_EQ(parser.statement_sql().sql(), "FROM slice");
  EXPECT_GT(parser.statement_end_offset(), 0u);
  ASSERT_TRUE(parser.Next());
  EXPECT_TRUE(std::holds_alternative<SqliteSql>(parser.statement()));
  const auto& plan = std::get<Pipeline>(statement).plan;
  EXPECT_THAT(pipeline::LogicalPlanToString(plan),
              HasSubstr("Scan(table slice)"));
}

TEST_F(PerfettoSqlParserTest, PipelineNeedsToBeAllowed) {
  PerfettoSqlParser parser(macros_, catalog_,
                           /*pipelines_allowed=*/false);
  parser.Reset(SqlSource::FromExecuteQuery("FROM slice"));
  ASSERT_FALSE(parser.Next());
  EXPECT_THAT(parser.status().message(),
              HasSubstr("Pipelines are not enabled"));
}

// One parser serves sources which differ in whether they may use a pipeline,
// so the permission has to follow the source rather than the parser.
TEST_F(PerfettoSqlParserTest, PipelinePermissionChangesWithTheSource) {
  PerfettoSqlParser parser(macros_, catalog_,
                           /*pipelines_allowed=*/false);
  parser.Reset(SqlSource::FromExecuteQuery("FROM slice"));
  ASSERT_FALSE(parser.Next());

  parser.SetPipelinesAllowed(true);
  parser.Reset(SqlSource::FromExecuteQuery("FROM slice"));
  ASSERT_TRUE(parser.Next()) << parser.status().message();

  parser.SetPipelinesAllowed(false);
  parser.Reset(SqlSource::FromExecuteQuery("FROM slice"));
  ASSERT_FALSE(parser.Next());
}

// The new keywords are not reserved and `|` `>` in an expression is still
// bitwise OR then a comparison.
TEST_F(PerfettoSqlParserTest, PipelineSyntaxDoesNotLeakIntoSql) {
  ASSERT_EQ(*Parse(SqlSource::FromExecuteQuery(
                "SELECT tree, accumulate, up, down FROM t")),
            std::vector<Statement>{Statement(SqliteSql{})});
  ASSERT_EQ(*Parse(SqlSource::FromExecuteQuery(
                "SELECT * FROM a JOIN b ON a.x | b.y > 3")),
            std::vector<Statement>{Statement(SqliteSql{})});
}

TEST_F(PerfettoSqlParserTest, CreatePerfettoView) {
  auto res = SqlSource::FromExecuteQuery(
      "CREATE PERFETTO VIEW foo AS SELECT 42 AS bar");
  PerfettoSqlParser parser(macros_, catalog_,
                           /*pipelines_allowed=*/false);
  parser.Reset(res);
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
  PerfettoSqlParser parser(macros_, catalog_,
                           /*pipelines_allowed=*/false);
  parser.Reset(res);
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
  PerfettoSqlParser parser(macros_, catalog_,
                           /*pipelines_allowed=*/false);
  parser.Reset(res);
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
  PerfettoSqlParser parser(macros_, catalog_,
                           /*pipelines_allowed=*/false);
  parser.Reset(res);
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
  PerfettoSqlParser parser(macros_, catalog_,
                           /*pipelines_allowed=*/false);
  parser.Reset(res);
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

// ---------------------------------------------------------------------------
// Macro expansion tests.
//
// The rest of the suite never invokes a macro (`macros_` stays empty), so the
// MacroRewriteBuilder code paths (`BuildForRewrite`, `BuildForUserMacro`,
// `BuildForArg`, `AuthoredSourceOf`, intrinsic dispatch) go uncovered there.
// These tests register macros directly and assert on the rewritten
// `statement_sql()`.  The `original_sql()` side asserts that the rewrite tree
// preserves the authored form of the statement (what `AsTraceback` would walk).
// ---------------------------------------------------------------------------

TEST_F(PerfettoSqlParserTest, ExpandsSimpleUserMacro) {
  RegisterMacro("one", {"x"}, "SELECT $x AS col");
  SqlSource out = ParseOne(SqlSource::FromExecuteQuery("one!(42)"));
  EXPECT_EQ(out.sql(), "SELECT 42 AS col");
  EXPECT_EQ(out.original_sql(), "one!(42)");
}

TEST_F(PerfettoSqlParserTest, ExpandsMacroWithMultipleParams) {
  RegisterMacro("pair", {"a", "b"}, "$a + $b + $a");
  SqlSource out = ParseOne(SqlSource::FromExecuteQuery("SELECT pair!(x, y)"));
  EXPECT_EQ(out.sql(), "SELECT x + y + x");
  EXPECT_EQ(out.original_sql(), "SELECT pair!(x, y)");
}

TEST_F(PerfettoSqlParserTest, ExpandsNestedLiteralMacroCall) {
  // `my_wrap`'s body literally contains a call to `my_inc` — exercises the
  // body-rooted nested-call path in BuildForUserMacro.
  RegisterMacro("my_inc", {"x"}, "($x + 1)");
  RegisterMacro("my_wrap", {"y"}, "SELECT $y + my_inc!(10)");
  SqlSource out = ParseOne(SqlSource::FromExecuteQuery("my_wrap!(5)"));
  EXPECT_EQ(out.sql(), "SELECT 5 + (10 + 1)");
}

TEST_F(PerfettoSqlParserTest, ExpandsMacroInsideArg) {
  // `my_wrap` receives a macro call `my_double!(5)` as its arg — exercises
  // the BuildForArg path: a child whose call-site lives inside a $param
  // substitution of the parent.
  RegisterMacro("my_double", {"x"}, "($x * 2)");
  RegisterMacro("my_wrap", {"y"}, "SELECT $y");
  SqlSource out =
      ParseOne(SqlSource::FromExecuteQuery("my_wrap!(my_double!(5))"));
  EXPECT_EQ(out.sql(), "SELECT (5 * 2)");
}

TEST_F(PerfettoSqlParserTest, ExpandsParamChainThroughMultipleMacros) {
  // `$a` in `my_ident` is substituted with `$b` from `my_wrap`, which is
  // itself substituted with `7` from the authored call site.  When we build
  // a SqlSource for the final `7`, AuthoredSourceOf must drill through the
  // chain my_ident ← my_wrap ← source.
  RegisterMacro("my_ident", {"a"}, "$a");
  RegisterMacro("my_wrap", {"b"}, "SELECT my_ident!($b)");
  SqlSource out = ParseOne(SqlSource::FromExecuteQuery("my_wrap!(7)"));
  EXPECT_EQ(out.sql(), "SELECT 7");
  // AsTraceback should reach all the way back to the authored "7" position
  // in the original source, not name an intermediate macro expansion buffer.
  std::string tb = out.AsTraceback(static_cast<uint32_t>(out.sql().find('7')));
  EXPECT_THAT(tb, testing::HasSubstr("my_wrap!(7)"));
}

TEST_F(PerfettoSqlParserTest, ExpandsMacroWithSubsumedParamSegment) {
  // `my_wrap`'s body calls `my_parens!($x)` — the `$x` $param segment's
  // body range is *inside* the literal `my_parens!(...)` body-call range.
  // The subsumption filter in BuildForUserMacro must drop the segment to
  // keep the Rewriter's invariant (no overlapping rewrites) satisfied.
  RegisterMacro("my_parens", {"y"}, "($y)");
  RegisterMacro("my_wrap", {"x"}, "SELECT my_parens!($x)");
  SqlSource out = ParseOne(SqlSource::FromExecuteQuery("my_wrap!(42)"));
  EXPECT_EQ(out.sql(), "SELECT (42)");
}

TEST_F(PerfettoSqlParserTest, ExpandsStringifyIntrinsic) {
  SqlSource out = ParseOne(
      SqlSource::FromExecuteQuery("SELECT __intrinsic_stringify!(foo bar)"));
  EXPECT_EQ(out.sql(), "SELECT 'foo bar'");
}

TEST_F(PerfettoSqlParserTest, ExpandsTokenApplyIntrinsic) {
  // token_apply!(m, (a, b, c)) -> m!(a), m!(b), m!(c)
  RegisterMacro("wrap", {"x"}, "WRAP($x)");
  SqlSource out = ParseOne(SqlSource::FromExecuteQuery(
      "SELECT __intrinsic_token_apply!(wrap, (1, 2, 3))"));
  EXPECT_EQ(out.sql(), "SELECT WRAP(1), WRAP(2), WRAP(3)");
}

TEST_F(PerfettoSqlParserTest, UnknownMacroSurfacesError) {
  auto parsed = Parse(SqlSource::FromExecuteQuery("undefined!(1)"));
  ASSERT_FALSE(parsed.status().ok());
}

TEST_F(PerfettoSqlParserTest, WrongMacroArgCountSurfacesError) {
  RegisterMacro("binary", {"a", "b"}, "$a + $b");
  auto parsed = Parse(SqlSource::FromExecuteQuery("binary!(1)"));
  ASSERT_FALSE(parsed.status().ok());
}

}  // namespace
}  // namespace perfetto::trace_processor
