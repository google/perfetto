/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/plugins/string_functions/sqlite3_str_split.h"

#include <sqlite3.h>
#include <memory>
#include <string>

#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::string_functions {
namespace {

class Sqlite3StrSplitTest : public ::testing::Test {
 public:
  Sqlite3StrSplitTest() {
    PerfettoSqlConnection::Initializer init;
    init.functions.push_back(MakeFunctionRegistration<StrSplit>(nullptr));
    connection_->Initialize(std::move(init));
  }

  const char* SplitStmt(const std::string& str,
                        const std::string& delim,
                        int field) {
    const std::string sql = "SELECT STR_SPLIT(\"" + str + "\", \"" + delim +
                            "\", " + std::to_string(field) + ");";
    auto result = connection_->ExecuteUntilLastStatement(
        SqlSource::FromExecuteQuery(sql));
    PERFETTO_CHECK(result.ok());
    last_result_ = std::make_unique<PerfettoSqlConnection::ExecutionResult>(
        std::move(*result));
    sqlite3_stmt* stmt = last_result_->stmt.sqlite_stmt();
    if (sqlite3_column_type(stmt, 0) == SQLITE_NULL) {
      return nullptr;
    }
    return reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  }

 protected:
  StringPool pool_;
  std::unique_ptr<PerfettoSqlConnection> connection_ =
      PerfettoSqlConnection::CreateConnectionToNewDatabase(&pool_, true);
  std::unique_ptr<PerfettoSqlConnection::ExecutionResult> last_result_;
};

TEST_F(Sqlite3StrSplitTest, SplitNoDelimiter) {
  ASSERT_STREQ(SplitStmt("abc", ":", 0), "abc");
  ASSERT_EQ(SplitStmt("abc", ":", 1), nullptr);
}

TEST_F(Sqlite3StrSplitTest, SplitSingleCharDelim) {
  ASSERT_STREQ(SplitStmt("a:bc", ":", 0), "a");
  ASSERT_STREQ(SplitStmt("a:bc", ":", 1), "bc");
  ASSERT_EQ(SplitStmt("a:bc", ":", 2), nullptr);
}

TEST_F(Sqlite3StrSplitTest, SplitInputConsecutiveDelim) {
  ASSERT_STREQ(SplitStmt("a::b::c", ":", 0), "a");
  ASSERT_STREQ(SplitStmt("a::b::c", ":", 1), "");
  ASSERT_STREQ(SplitStmt("a::b::c", ":", 2), "b");
  ASSERT_STREQ(SplitStmt("a::b::c", ":", 3), "");
  ASSERT_STREQ(SplitStmt("a::b::c", ":", 4), "c");
  ASSERT_EQ(SplitStmt("a::b::c", ":", 5), nullptr);
}

TEST_F(Sqlite3StrSplitTest, SplitStringDelim) {
  ASSERT_STREQ(SplitStmt("abczzdefzzghi", "zz", 0), "abc");
  ASSERT_STREQ(SplitStmt("abczzdefzzghi", "zz", 1), "def");
  ASSERT_STREQ(SplitStmt("abczzdefzzghi", "zz", 2), "ghi");
}

TEST_F(Sqlite3StrSplitTest, SplitEmptyInput) {
  ASSERT_STREQ(SplitStmt("", "zz", 0), "");
  ASSERT_EQ(SplitStmt("", "zz", 1), nullptr);
  ASSERT_EQ(SplitStmt("", "zz", 1000), nullptr);
}

}  // namespace
}  // namespace perfetto::trace_processor::string_functions
