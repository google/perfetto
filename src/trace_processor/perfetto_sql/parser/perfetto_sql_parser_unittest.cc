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
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/perfetto_sql/parser/function_util.h"
#include "src/trace_processor/perfetto_sql/parser/perfetto_sql_test_utils.h"
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
    PerfettoSqlParser parser(std::move(sql), macros_);
    PERFETTO_CHECK(parser.Next());
    PERFETTO_CHECK(parser.status().ok());
    SqlSource out = parser.statement_sql();
    PERFETTO_CHECK(!parser.Next());
    return out;
  }

  base::FlatHashMap<std::string, PerfettoSqlParser::Macro> macros_;
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
