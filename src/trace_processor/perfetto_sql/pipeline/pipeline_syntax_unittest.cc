/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_processor/perfetto_sql/pipeline/pipeline_syntax.h"

#include <optional>
#include <string>
#include <variant>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::pipeline {
namespace {

using testing::HasSubstr;

base::StatusOr<PipelineSyntax> Parse(const std::string& sql) {
  return ParsePipeline(SqlSource::FromExecuteQuery(sql));
}

const TreeAccumulate& AccumulateAt(const PipelineSyntax& syntax, size_t i) {
  return std::get<TreeAccumulate>(syntax.stages[i].op);
}

TEST(PipelineSyntaxTest, AFromAloneHasNoStages) {
  auto syntax = Parse("FROM slice");
  ASSERT_TRUE(syntax.ok()) << syntax.status().message();
  EXPECT_EQ(syntax->from.sql(), "slice");
  EXPECT_EQ(syntax->from_name, "slice");
  EXPECT_TRUE(syntax->stages.empty());
}

TEST(PipelineSyntaxTest, TheFromClauseIsKeptAsWritten) {
  auto syntax = Parse(
      "FROM (SELECT id, parent_id FROM slice WHERE name GLOB 'a|>b') AS s "
      "|> TREE ACCUMULATE UP SUM(dur) AS total");
  ASSERT_TRUE(syntax.ok()) << syntax.status().message();
  EXPECT_EQ(syntax->from.sql(),
            "(SELECT id, parent_id FROM slice WHERE name GLOB 'a|>b') AS s");
  EXPECT_EQ(syntax->from_name, std::nullopt);
  ASSERT_EQ(syntax->stages.size(), 1u);
}

TEST(PipelineSyntaxTest, TreeAccumulate) {
  auto syntax = Parse(
      "FROM slice\n"
      "|> tree accumulate up sum(dur) AS total, SUM(\"self\") AS [self total]\n"
      "|> TREE ACCUMULATE DOWN SUM(depth) AS d");
  ASSERT_TRUE(syntax.ok()) << syntax.status().message();
  ASSERT_EQ(syntax->stages.size(), 2u);

  const TreeAccumulate& up = AccumulateAt(*syntax, 0);
  EXPECT_EQ(up.direction, TreeAccumulate::Direction::kUp);
  ASSERT_EQ(up.aggregates.size(), 2u);
  EXPECT_EQ(up.aggregates[0].function, "SUM");
  EXPECT_EQ(up.aggregates[0].column, "dur");
  EXPECT_EQ(up.aggregates[0].name, "total");
  EXPECT_EQ(up.aggregates[0].sql.sql(), "sum(dur) AS total");
  EXPECT_EQ(up.aggregates[1].column, "self");
  EXPECT_EQ(up.aggregates[1].name, "self total");

  const TreeAccumulate& down = AccumulateAt(*syntax, 1);
  EXPECT_EQ(down.direction, TreeAccumulate::Direction::kDown);
  ASSERT_EQ(down.aggregates.size(), 1u);
  EXPECT_EQ(syntax->stages[1].sql.sql(),
            "TREE ACCUMULATE DOWN SUM(depth) AS d");
}

TEST(PipelineSyntaxTest, AStarArgumentHasNoColumn) {
  auto syntax = Parse("FROM slice |> TREE ACCUMULATE UP COUNT(*) AS size");
  ASSERT_TRUE(syntax.ok()) << syntax.status().message();
  EXPECT_EQ(AccumulateAt(*syntax, 0).aggregates[0].function, "COUNT");
  EXPECT_EQ(AccumulateAt(*syntax, 0).aggregates[0].column, std::nullopt);
}

// A pipe inside parentheses belongs to whatever the parentheses hold.
TEST(PipelineSyntaxTest, PipesInsideParenthesesDoNotSplit) {
  auto syntax = Parse("FROM (FROM slice |> TREE ACCUMULATE UP SUM(dur) AS a)");
  ASSERT_TRUE(syntax.ok()) << syntax.status().message();
  EXPECT_TRUE(syntax->stages.empty());
}

TEST(PipelineSyntaxTest, APipeIsOneToken) {
  auto syntax = Parse("FROM slice | > TREE ACCUMULATE UP SUM(dur) AS a");
  ASSERT_TRUE(syntax.ok()) << syntax.status().message();
  EXPECT_TRUE(syntax->stages.empty());
}

TEST(PipelineSyntaxTest, Errors) {
  EXPECT_THAT(Parse("SELECT 1").status().message(),
              HasSubstr("must start with FROM"));
  EXPECT_THAT(
      Parse("FROM |> TREE ACCUMULATE UP SUM(a) AS b").status().message(),
      HasSubstr("expected a table, view or subquery"));
  EXPECT_THAT(Parse("FROM slice |>").status().message(),
              HasSubstr("Expected a pipe operator"));
  EXPECT_THAT(Parse("FROM slice |> WHERE dur > 0").status().message(),
              HasSubstr("Unsupported pipe operator 'WHERE'"));
  EXPECT_THAT(Parse("FROM slice |> TREE KEEP IF").status().message(),
              HasSubstr("Unsupported pipe operator 'TREE KEEP'"));
  EXPECT_THAT(
      Parse("FROM slice |> TREE ACCUMULATE SUM(a) AS b").status().message(),
      HasSubstr("expected UP or DOWN"));
  EXPECT_THAT(Parse("FROM slice |> TREE ACCUMULATE UP SUM(a + 1) AS b")
                  .status()
                  .message(),
              HasSubstr("expected ')'"));
  EXPECT_THAT(
      Parse("FROM slice |> TREE ACCUMULATE UP SUM(a)").status().message(),
      HasSubstr("expected AS"));
  EXPECT_THAT(Parse("FROM slice |> TREE ACCUMULATE UP SUM(a) AS b c")
                  .status()
                  .message(),
              HasSubstr("expected ','"));
}

// Errors point at where in the query the problem is.
TEST(PipelineSyntaxTest, ErrorsCarryATraceback) {
  auto syntax = Parse("FROM slice |> TREE ACCUMULATE SIDEWAYS SUM(a) AS b");
  ASSERT_FALSE(syntax.ok());
  EXPECT_THAT(syntax.status().message(), HasSubstr("^"));
}

}  // namespace
}  // namespace perfetto::trace_processor::pipeline
