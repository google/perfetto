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

#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

using Result = PerfettoSqlParser::Statement;
using SqliteSql = PerfettoSqlParser::SqliteSql;

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

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
