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

#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_preprocessor.h"

#include <string>

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_test_utils.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

using Macro = PerfettoSqlPreprocessor::Macro;

class PerfettoSqlPreprocessorUnittest : public ::testing::Test {
 protected:
  base::FlatHashMap<std::string, PerfettoSqlPreprocessor::Macro> macros_;
};

TEST_F(PerfettoSqlPreprocessorUnittest, Empty) {
  PerfettoSqlPreprocessor preprocessor(SqlSource::FromExecuteQuery(""),
                                       macros_);
  ASSERT_FALSE(preprocessor.NextStatement());
  ASSERT_TRUE(preprocessor.status().ok());
}

TEST_F(PerfettoSqlPreprocessorUnittest, SemiColonTerminatedStatement) {
  auto source = SqlSource::FromExecuteQuery("SELECT * FROM slice;");
  PerfettoSqlPreprocessor preprocessor(source, macros_);
  ASSERT_TRUE(preprocessor.NextStatement());
  ASSERT_EQ(preprocessor.statement(),
            FindSubstr(source, "SELECT * FROM slice"));
  ASSERT_FALSE(preprocessor.NextStatement());
  ASSERT_TRUE(preprocessor.status().ok());
}

TEST_F(PerfettoSqlPreprocessorUnittest, IgnoreOnlySpace) {
  auto source = SqlSource::FromExecuteQuery(" ; SELECT * FROM s; ; ;");
  PerfettoSqlPreprocessor preprocessor(source, macros_);
  ASSERT_TRUE(preprocessor.NextStatement());
  ASSERT_EQ(preprocessor.statement(), FindSubstr(source, "SELECT * FROM s"));
  ASSERT_FALSE(preprocessor.NextStatement());
  ASSERT_TRUE(preprocessor.status().ok());
}

TEST_F(PerfettoSqlPreprocessorUnittest, MultipleStmts) {
  auto source =
      SqlSource::FromExecuteQuery("SELECT * FROM slice; SELECT * FROM s");
  PerfettoSqlPreprocessor preprocessor(source, macros_);
  ASSERT_TRUE(preprocessor.NextStatement());
  ASSERT_EQ(preprocessor.statement(),
            FindSubstr(source, "SELECT * FROM slice"));
  ASSERT_TRUE(preprocessor.NextStatement());
  ASSERT_EQ(preprocessor.statement(), FindSubstr(source, "SELECT * FROM s"));
  ASSERT_FALSE(preprocessor.NextStatement());
  ASSERT_TRUE(preprocessor.status().ok());
}

TEST_F(PerfettoSqlPreprocessorUnittest, CreateMacro) {
  auto source = SqlSource::FromExecuteQuery(
      "CREATE PERFETTO MACRO foo(a, b) AS SELECT $a + $b");
  PerfettoSqlPreprocessor preprocessor(source, macros_);
  ASSERT_TRUE(preprocessor.NextStatement());
  ASSERT_EQ(
      preprocessor.statement(),
      FindSubstr(source, "CREATE PERFETTO MACRO foo(a, b) AS SELECT $a + $b"));
  ASSERT_FALSE(preprocessor.NextStatement());
  ASSERT_TRUE(preprocessor.status().ok());
}

TEST_F(PerfettoSqlPreprocessorUnittest, SingleMacro) {
  auto foo = SqlSource::FromExecuteQuery(
      "CREATE PERFETTO MACRO foo(a Expr, b Expr) Returns Expr AS "
      "SELECT $a + $b");
  macros_.Insert(
      "foo",
      Macro{false, "foo", {"a", "b"}, FindSubstr(foo, "SELECT $a + $b")});

  auto source = SqlSource::FromExecuteQuery(
      "foo!((select s.ts + r.dur from s, r), 1234); SELECT 1");
  PerfettoSqlPreprocessor preprocessor(source, macros_);
  ASSERT_TRUE(preprocessor.NextStatement());
  ASSERT_EQ(preprocessor.statement().AsTraceback(0),
            "Fully expanded statement\n"
            "  SELECT (select s.ts + r.dur from s, r) + 1234\n"
            "  ^\n"
            "Traceback (most recent call last):\n"
            "  File \"stdin\" line 1 col 1\n"
            "    foo!((select s.ts + r.dur from s, r), 1234)\n"
            "    ^\n"
            "  File \"stdin\" line 1 col 59\n"
            "    SELECT $a + $b\n"
            "    ^\n");
  ASSERT_EQ(preprocessor.statement().AsTraceback(7),
            "Fully expanded statement\n"
            "  SELECT (select s.ts + r.dur from s, r) + 1234\n"
            "         ^\n"
            "Traceback (most recent call last):\n"
            "  File \"stdin\" line 1 col 1\n"
            "    foo!((select s.ts + r.dur from s, r), 1234)\n"
            "    ^\n"
            "  File \"stdin\" line 1 col 66\n"
            "    SELECT $a + $b\n"
            "           ^\n"
            "  File \"stdin\" line 1 col 6\n"
            "    (select s.ts + r.dur from s, r)\n"
            "    ^\n");
  ASSERT_EQ(preprocessor.statement().sql(),
            "SELECT (select s.ts + r.dur from s, r) + 1234");
  ASSERT_TRUE(preprocessor.NextStatement());
  ASSERT_EQ(preprocessor.statement(), FindSubstr(source, "SELECT 1"));
  ASSERT_FALSE(preprocessor.NextStatement());
  ASSERT_TRUE(preprocessor.status().ok());
}

TEST_F(PerfettoSqlPreprocessorUnittest, NestedMacro) {
  auto foo = SqlSource::FromExecuteQuery(
      "CREATE PERFETTO MACRO foo(a Expr, b Expr) Returns Expr AS $a + $b");
  macros_.Insert("foo", Macro{
                            false,
                            "foo",
                            {"a", "b"},
                            FindSubstr(foo, "$a + $b"),
                        });

  auto bar = SqlSource::FromExecuteQuery(
      "CREATE PERFETTO MACRO bar(a, b) Returns Expr AS "
      "tfoo!($a, $b) + foo!($b, $a)");
  macros_.Insert("bar", Macro{
                            false,
                            "bar",
                            {"a", "b"},
                            FindSubstr(bar, "foo!($a, $b) + foo!($b, $a)"),
                        });

  auto source = SqlSource::FromExecuteQuery(
      "SELECT bar!((select s.ts + r.dur from s, r), 1234); SELECT 1");
  PerfettoSqlPreprocessor preprocessor(source, macros_);
  ASSERT_TRUE(preprocessor.NextStatement());
  ASSERT_EQ(preprocessor.statement().sql(),
            "SELECT (select s.ts + r.dur from s, r) + 1234 + 1234 + "
            "(select s.ts + r.dur from s, r)");
  ASSERT_TRUE(preprocessor.NextStatement());
  ASSERT_EQ(preprocessor.statement().sql(), "SELECT 1");
}

TEST_F(PerfettoSqlPreprocessorUnittest, Stringify) {
  auto foo = SqlSource::FromExecuteQuery(
      "CREATE PERFETTO MACRO sf(a Expr, b Expr) Returns Expr AS "
      "__intrinsic_stringify!($a + $b)");
  macros_.Insert("sf", Macro{
                           false,
                           "sf",
                           {"a", "b"},
                           FindSubstr(foo, "__intrinsic_stringify!($a + $b)"),
                       });

  {
    auto source =
        SqlSource::FromExecuteQuery("__intrinsic_stringify!(foo bar baz)");
    PerfettoSqlPreprocessor preprocessor(source, macros_);
    ASSERT_TRUE(preprocessor.NextStatement())
        << preprocessor.status().message();
    ASSERT_EQ(preprocessor.statement().sql(), "'foo bar baz'");
    ASSERT_FALSE(preprocessor.NextStatement());
  }

  {
    auto source = SqlSource::FromExecuteQuery("sf!(1, 2)");
    PerfettoSqlPreprocessor preprocessor(source, macros_);
    ASSERT_TRUE(preprocessor.NextStatement())
        << preprocessor.status().message();
    ASSERT_EQ(preprocessor.statement().sql(), "'1 + 2'");
    ASSERT_FALSE(preprocessor.NextStatement());
  }

  {
    auto source = SqlSource::FromExecuteQuery("__intrinsic_stringify!()");
    PerfettoSqlPreprocessor preprocessor(source, macros_);
    ASSERT_FALSE(preprocessor.NextStatement());
    ASSERT_EQ(preprocessor.status().message(),
              "Traceback (most recent call last):\n"
              "  File \"stdin\" line 1 col 1\n"
              "    __intrinsic_stringify!()\n"
              "    ^\n"
              "stringify: stringify must have exactly one argument");
  }
}

TEST_F(PerfettoSqlPreprocessorUnittest, ZipJoin) {
  auto foo = SqlSource::FromExecuteQuery(
      "CREATE PERFETTO MACRO G(a Expr, b Expr) Returns Expr AS $a AS $b");
  macros_.Insert("G", Macro{
                          false,
                          "G",
                          {"a", "b"},
                          FindSubstr(foo, "$a AS $b"),
                      });

  auto zj = SqlSource::FromExecuteQuery(
      "CREATE PERFETTO MACRO ZJ(a Expr, b Expr, c Expr, d Expr) Returns Expr "
      "AS __intrinsic_token_zip_join!($a, $b, $c, $d)");
  macros_.Insert(
      "ZJ", Macro{
                false,
                "ZJ",
                {"a", "b", "c", "d"},
                FindSubstr(zj, "__intrinsic_token_zip_join!($a, $b, $c, $d)"),
            });
  {
    auto source = SqlSource::FromExecuteQuery(
        "__intrinsic_token_zip_join!((foo, bar), (baz, bat), G, AND)");
    PerfettoSqlPreprocessor preprocessor(source, macros_);
    ASSERT_TRUE(preprocessor.NextStatement())
        << preprocessor.status().message();
    ASSERT_EQ(preprocessor.statement().sql(), "foo AS baz AND bar AS bat");
    ASSERT_FALSE(preprocessor.NextStatement());
  }
  {
    auto source = SqlSource::FromExecuteQuery(
        "__intrinsic_token_zip_join!((foo, bar), (baz, bat, bada), G, AND)");
    PerfettoSqlPreprocessor preprocessor(source, macros_);
    ASSERT_TRUE(preprocessor.NextStatement())
        << preprocessor.status().message();
    ASSERT_EQ(preprocessor.statement().sql(), "foo AS baz AND bar AS bat");
    ASSERT_FALSE(preprocessor.NextStatement());
  }
  {
    auto source = SqlSource::FromExecuteQuery(
        "__intrinsic_token_zip_join!((foo, bar), (baz, bat, bada), G, "
        "__intrinsic_token_comma!())");
    PerfettoSqlPreprocessor preprocessor(source, macros_);
    ASSERT_TRUE(preprocessor.NextStatement())
        << preprocessor.status().message();
    ASSERT_EQ(preprocessor.statement().sql(), "foo AS baz , bar AS bat");
    ASSERT_FALSE(preprocessor.NextStatement());
  }
  {
    auto source = SqlSource::FromExecuteQuery(
        "ZJ!((foo, bar), (baz, bat, bada), G, __intrinsic_token_comma!())");
    PerfettoSqlPreprocessor preprocessor(source, macros_);
    ASSERT_TRUE(preprocessor.NextStatement())
        << preprocessor.status().message();
    ASSERT_EQ(preprocessor.statement().sql(), "foo AS baz , bar AS bat");
    ASSERT_FALSE(preprocessor.NextStatement());
  }
}

}  // namespace
}  // namespace perfetto::trace_processor
