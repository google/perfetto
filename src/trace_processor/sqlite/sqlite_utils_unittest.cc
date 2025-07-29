/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "src/trace_processor/sqlite/sqlite_utils.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_processor/sqlite/scoped_db.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::sqlite::utils {

namespace {
using base::gtest_matchers::IsError;

class GetColumnsForTableTest : public ::testing::Test {
 public:
  GetColumnsForTableTest() {
    sqlite3* db = nullptr;
    PERFETTO_CHECK(sqlite3_initialize() == SQLITE_OK);
    PERFETTO_CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
    db_.reset(db);
  }

  void PrepareValidStatement(const std::string& sql) {
    int size = static_cast<int>(sql.size());
    sqlite3_stmt* stmt;
    ASSERT_EQ(sqlite3_prepare_v2(*db_, sql.c_str(), size, &stmt, nullptr),
              SQLITE_OK);
    stmt_.reset(stmt);
  }

  void RunStatement(const std::string& sql) {
    PrepareValidStatement(sql);
    ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_DONE);
  }

 protected:
  ScopedDb db_;
  ScopedStmt stmt_;
};

TEST_F(GetColumnsForTableTest, ValidInput) {
  RunStatement("CREATE TABLE foo (name STRING, ts INT, dur INT);");
  std::vector<std::pair<SqlValue::Type, std::string>> columns;
  ASSERT_OK(sqlite::utils::GetColumnsForTable(*db_, "foo", columns));
}

TEST_F(GetColumnsForTableTest, UnknownType) {
  // Currently GetColumnsForTable does not work with tables containing types it
  // doesn't recognise. This just ensures that the query fails rather than
  // crashing.
  RunStatement("CREATE TABLE foo (name NUM, ts INT, dur INT);");
  std::vector<std::pair<SqlValue::Type, std::string>> columns;
  ASSERT_THAT(sqlite::utils::GetColumnsForTable(*db_, "foo", columns),
              IsError());
}

TEST_F(GetColumnsForTableTest, UnknownTableName) {
  std::vector<std::pair<SqlValue::Type, std::string>> columns;
  ASSERT_THAT(sqlite::utils::GetColumnsForTable(*db_, "unknowntable", columns),
              IsError());
}

}  // namespace
}  // namespace perfetto::trace_processor::sqlite::utils
