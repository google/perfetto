/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/util/sql_module_doc_parser.h"

#include <string>
#include <string_view>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::stdlib_doc {
namespace {

ParsedModule Parse(std::string_view sql) {
  return ParseStdlibModule(sql.data(), static_cast<uint32_t>(sql.size()));
}

TEST(SqlModuleDocParserTest, Table) {
  auto m = Parse(R"(
-- A sample table.
CREATE PERFETTO TABLE my_table(
  -- The timestamp.
  ts LONG,
  -- The name.
  name STRING
) AS SELECT 1;
)");
  ASSERT_TRUE(m.errors.empty());
  ASSERT_EQ(m.table_views.size(), 1u);
  const auto& tv = m.table_views[0];
  EXPECT_EQ(tv.name, "my_table");
  EXPECT_EQ(tv.type, "TABLE");
  EXPECT_TRUE(tv.exposed);
  EXPECT_EQ(tv.description, "A sample table.");
  ASSERT_EQ(tv.columns.size(), 2u);
  EXPECT_EQ(tv.columns[0].name, "ts");
  EXPECT_EQ(tv.columns[0].type, "LONG");
  EXPECT_EQ(tv.columns[0].description, "The timestamp.");
  EXPECT_EQ(tv.columns[1].name, "name");
  EXPECT_EQ(tv.columns[1].type, "STRING");
  EXPECT_EQ(tv.columns[1].description, "The name.");
}

TEST(SqlModuleDocParserTest, View) {
  auto m = Parse(R"(
-- A sample view.
CREATE PERFETTO VIEW my_view(
  -- Event id.
  id LONG
) AS SELECT 1;
)");
  ASSERT_TRUE(m.errors.empty());
  ASSERT_EQ(m.table_views.size(), 1u);
  EXPECT_EQ(m.table_views[0].type, "VIEW");
  EXPECT_EQ(m.table_views[0].name, "my_view");
  EXPECT_EQ(m.table_views[0].description, "A sample view.");
}

TEST(SqlModuleDocParserTest, ScalarFunction) {
  auto m = Parse(R"(
-- Computes the sum.
CREATE PERFETTO FUNCTION my_fn(
  -- First value.
  x LONG,
  -- Second value.
  y LONG
)
-- The result.
RETURNS LONG AS
SELECT $x + $y;
)");
  ASSERT_TRUE(m.errors.empty());
  ASSERT_EQ(m.functions.size(), 1u);
  const auto& fn = m.functions[0];
  EXPECT_EQ(fn.name, "my_fn");
  EXPECT_FALSE(fn.is_table_function);
  EXPECT_TRUE(fn.exposed);
  EXPECT_EQ(fn.description, "Computes the sum.");
  EXPECT_EQ(fn.return_type, "LONG");
  EXPECT_EQ(fn.return_description, "The result.");
  ASSERT_EQ(fn.args.size(), 2u);
  EXPECT_EQ(fn.args[0].name, "x");
  EXPECT_EQ(fn.args[0].type, "LONG");
  EXPECT_EQ(fn.args[0].description, "First value.");
  EXPECT_EQ(fn.args[1].name, "y");
  EXPECT_EQ(fn.args[1].type, "LONG");
  EXPECT_EQ(fn.args[1].description, "Second value.");
}

TEST(SqlModuleDocParserTest, TableFunction) {
  auto m = Parse(R"(
-- Returns rows.
CREATE PERFETTO FUNCTION my_table_fn(
  -- Filter value.
  filter STRING
)
RETURNS TABLE(
  -- Row id.
  id LONG,
  -- Row value.
  val STRING
) AS
SELECT 1, 'a';
)");
  ASSERT_TRUE(m.errors.empty());
  ASSERT_EQ(m.functions.size(), 1u);
  const auto& fn = m.functions[0];
  EXPECT_EQ(fn.name, "my_table_fn");
  EXPECT_TRUE(fn.is_table_function);
  EXPECT_EQ(fn.return_type, "TABLE");
  EXPECT_EQ(fn.description, "Returns rows.");
  ASSERT_EQ(fn.args.size(), 1u);
  EXPECT_EQ(fn.args[0].name, "filter");
  ASSERT_EQ(fn.columns.size(), 2u);
  EXPECT_EQ(fn.columns[0].name, "id");
  EXPECT_EQ(fn.columns[0].type, "LONG");
  EXPECT_EQ(fn.columns[0].description, "Row id.");
  EXPECT_EQ(fn.columns[1].name, "val");
  EXPECT_EQ(fn.columns[1].type, "STRING");
  EXPECT_EQ(fn.columns[1].description, "Row value.");
}

TEST(SqlModuleDocParserTest, Macro) {
  auto m = Parse(R"(
-- Wraps an expression.
CREATE PERFETTO MACRO my_macro(
  -- The expression.
  x Expr
)
-- The wrapped result.
RETURNS Expr AS
$x;
)");
  ASSERT_TRUE(m.errors.empty());
  ASSERT_EQ(m.macros.size(), 1u);
  const auto& macro = m.macros[0];
  EXPECT_EQ(macro.name, "my_macro");
  EXPECT_TRUE(macro.exposed);
  EXPECT_EQ(macro.description, "Wraps an expression.");
  EXPECT_EQ(macro.return_type, "Expr");
  EXPECT_EQ(macro.return_description, "The wrapped result.");
  ASSERT_EQ(macro.args.size(), 1u);
  EXPECT_EQ(macro.args[0].name, "x");
  EXPECT_EQ(macro.args[0].type, "Expr");
  EXPECT_EQ(macro.args[0].description, "The expression.");
}

TEST(SqlModuleDocParserTest, InternalNamesNotExposed) {
  auto m = Parse(R"(
CREATE PERFETTO TABLE _internal_table(id LONG) AS SELECT 1;
CREATE PERFETTO FUNCTION _internal_fn(x LONG) RETURNS LONG AS SELECT $x;
CREATE PERFETTO MACRO _internal_macro(x Expr) RETURNS Expr AS $x;
)");
  ASSERT_TRUE(m.errors.empty());
  ASSERT_EQ(m.table_views.size(), 1u);
  EXPECT_FALSE(m.table_views[0].exposed);
  ASSERT_EQ(m.functions.size(), 1u);
  EXPECT_FALSE(m.functions[0].exposed);
  ASSERT_EQ(m.macros.size(), 1u);
  EXPECT_FALSE(m.macros[0].exposed);
}

TEST(SqlModuleDocParserTest, LicenseHeaderExcluded) {
  // A blank line between the license block and the doc comment means only
  // the doc comment should be used as the description.
  auto m = Parse(R"(
-- Copyright 2025 Acme Corp.
-- Licensed under Apache 2.0.

-- The real description.
CREATE PERFETTO TABLE t(id LONG) AS SELECT 1;
)");
  ASSERT_TRUE(m.errors.empty());
  ASSERT_EQ(m.table_views.size(), 1u);
  EXPECT_EQ(m.table_views[0].description, "The real description.");
}

TEST(SqlModuleDocParserTest, MultiLineDescription) {
  auto m = Parse(R"(
-- First line.
-- Second line.
CREATE PERFETTO TABLE t(id LONG) AS SELECT 1;
)");
  ASSERT_TRUE(m.errors.empty());
  ASSERT_EQ(m.table_views.size(), 1u);
  EXPECT_EQ(m.table_views[0].description, "First line. Second line.");
}

TEST(SqlModuleDocParserTest, MultipleObjects) {
  auto m = Parse(R"(
-- Table one.
CREATE PERFETTO TABLE t1(id LONG) AS SELECT 1;

-- Table two.
CREATE PERFETTO TABLE t2(id LONG) AS SELECT 1;

-- Function one.
CREATE PERFETTO FUNCTION f1(x LONG) RETURNS LONG AS SELECT $x;
)");
  ASSERT_TRUE(m.errors.empty());
  ASSERT_EQ(m.table_views.size(), 2u);
  EXPECT_EQ(m.table_views[0].description, "Table one.");
  EXPECT_EQ(m.table_views[1].description, "Table two.");
  ASSERT_EQ(m.functions.size(), 1u);
  EXPECT_EQ(m.functions[0].description, "Function one.");
}

TEST(SqlModuleDocParserTest, NoDescriptionIsEmpty) {
  auto m = Parse(R"(
CREATE PERFETTO TABLE t(id LONG) AS SELECT 1;
)");
  ASSERT_TRUE(m.errors.empty());
  ASSERT_EQ(m.table_views.size(), 1u);
  EXPECT_EQ(m.table_views[0].description, "");
}

TEST(SqlModuleDocParserTest, DelegatingFunction) {
  auto m = Parse(R"(
-- Alias for my_fn.
CREATE PERFETTO FUNCTION my_alias(
  -- The value.
  x LONG
)
RETURNS LONG
DELEGATES TO my_fn;
)");
  ASSERT_TRUE(m.errors.empty());
  ASSERT_EQ(m.functions.size(), 1u);
  const auto& fn = m.functions[0];
  EXPECT_EQ(fn.name, "my_alias");
  EXPECT_FALSE(fn.is_table_function);
  EXPECT_TRUE(fn.exposed);
  EXPECT_EQ(fn.description, "Alias for my_fn.");
  EXPECT_EQ(fn.return_type, "LONG");
  ASSERT_EQ(fn.args.size(), 1u);
  EXPECT_EQ(fn.args[0].name, "x");
  EXPECT_EQ(fn.args[0].description, "The value.");
}

TEST(SqlModuleDocParserTest, NoArgDescriptionIsEmpty) {
  auto m = Parse(R"(
CREATE PERFETTO FUNCTION f(x LONG) RETURNS LONG AS SELECT $x;
)");
  ASSERT_TRUE(m.errors.empty());
  ASSERT_EQ(m.functions.size(), 1u);
  ASSERT_EQ(m.functions[0].args.size(), 1u);
  EXPECT_EQ(m.functions[0].args[0].description, "");
}

TEST(SqlModuleDocParserTest, EmptyInput) {
  auto m = Parse("");
  EXPECT_TRUE(m.errors.empty());
  EXPECT_TRUE(m.table_views.empty());
  EXPECT_TRUE(m.functions.empty());
  EXPECT_TRUE(m.macros.empty());
}

TEST(SqlModuleDocParserTest, CommentOnlyInput) {
  auto m = Parse("-- Just a comment, no statements.");
  EXPECT_TRUE(m.errors.empty());
  EXPECT_TRUE(m.table_views.empty());
  EXPECT_TRUE(m.functions.empty());
  EXPECT_TRUE(m.macros.empty());
}

TEST(SqlModuleDocParserTest, ParseErrorRecorded) {
  auto m = Parse("THIS IS NOT VALID SQL @@@@;");
  EXPECT_FALSE(m.errors.empty());
}

}  // namespace
}  // namespace perfetto::trace_processor::stdlib_doc
