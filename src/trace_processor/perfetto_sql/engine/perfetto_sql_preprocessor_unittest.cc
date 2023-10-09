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

#include <optional>
#include <string>

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_test_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

using Macro = PerfettoSqlPreprocessor::Macro;

class PerfettoSqlPreprocessorUnittest : public ::testing::Test {};

TEST_F(PerfettoSqlPreprocessorUnittest, Empty) {
  PerfettoSqlPreprocessor preprocessor(SqlSource::FromExecuteQuery(""));
  ASSERT_FALSE(preprocessor.NextStatement());
  ASSERT_TRUE(preprocessor.status().ok());
}

TEST_F(PerfettoSqlPreprocessorUnittest, SemiColonTerminatedStatement) {
  auto source = SqlSource::FromExecuteQuery("SELECT * FROM slice;");
  PerfettoSqlPreprocessor preprocessor(source);
  ASSERT_TRUE(preprocessor.NextStatement());
  ASSERT_EQ(preprocessor.statement(),
            FindSubstr(source, "SELECT * FROM slice"));
  ASSERT_FALSE(preprocessor.NextStatement());
  ASSERT_TRUE(preprocessor.status().ok());
}

TEST_F(PerfettoSqlPreprocessorUnittest, IgnoreOnlySpace) {
  auto source = SqlSource::FromExecuteQuery(" ; SELECT * FROM s; ; ;");
  PerfettoSqlPreprocessor preprocessor(source);
  ASSERT_TRUE(preprocessor.NextStatement());
  ASSERT_EQ(preprocessor.statement(), FindSubstr(source, "SELECT * FROM s"));
  ASSERT_FALSE(preprocessor.NextStatement());
  ASSERT_TRUE(preprocessor.status().ok());
}

TEST_F(PerfettoSqlPreprocessorUnittest, MultipleStmts) {
  auto source =
      SqlSource::FromExecuteQuery("SELECT * FROM slice; SELECT * FROM s");
  PerfettoSqlPreprocessor preprocessor(source);
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
  PerfettoSqlPreprocessor preprocessor(source);
  ASSERT_TRUE(preprocessor.NextStatement());
  ASSERT_EQ(
      preprocessor.statement(),
      FindSubstr(source, "CREATE PERFETTO MACRO foo(a, b) AS SELECT $a + $b"));
  ASSERT_FALSE(preprocessor.NextStatement());
  ASSERT_TRUE(preprocessor.status().ok());
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
