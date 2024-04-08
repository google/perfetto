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

TEST(SqliteUtilsTest, ExtractFromSqlValueInt32) {
  std::optional<int32_t> int32;

  static constexpr int64_t kMin = std::numeric_limits<int32_t>::min();
  static constexpr int64_t kMax = std::numeric_limits<int32_t>::max();

  ASSERT_TRUE(ExtractFromSqlValue(SqlValue::Long(1234), int32).ok());
  ASSERT_EQ(*int32, 1234);

  ASSERT_TRUE(ExtractFromSqlValue(SqlValue::Long(kMin), int32).ok());
  ASSERT_EQ(*int32, kMin);

  ASSERT_TRUE(ExtractFromSqlValue(SqlValue::Long(kMax), int32).ok());
  ASSERT_EQ(*int32, kMax);

  ASSERT_TRUE(ExtractFromSqlValue(SqlValue(), int32).ok());
  ASSERT_FALSE(int32.has_value());

  ASSERT_FALSE(ExtractFromSqlValue(SqlValue::Long(kMax + 1), int32).ok());
  ASSERT_FALSE(ExtractFromSqlValue(SqlValue::Double(1.0), int32).ok());
  ASSERT_FALSE(ExtractFromSqlValue(SqlValue::String("foo"), int32).ok());
}

TEST(SqliteUtilsTest, ExtractFromSqlValueUint32) {
  std::optional<uint32_t> uint32;

  static constexpr int64_t kMin = std::numeric_limits<uint32_t>::min();
  static constexpr int64_t kMax = std::numeric_limits<uint32_t>::max();

  ASSERT_TRUE(ExtractFromSqlValue(SqlValue::Long(1234), uint32).ok());
  ASSERT_EQ(*uint32, 1234u);

  ASSERT_TRUE(ExtractFromSqlValue(SqlValue::Long(kMin), uint32).ok());
  ASSERT_EQ(*uint32, kMin);

  ASSERT_TRUE(ExtractFromSqlValue(SqlValue::Long(kMax), uint32).ok());
  ASSERT_EQ(*uint32, kMax);

  ASSERT_TRUE(ExtractFromSqlValue(SqlValue(), uint32).ok());
  ASSERT_FALSE(uint32.has_value());

  ASSERT_FALSE(ExtractFromSqlValue(SqlValue::Long(kMax + 1), uint32).ok());
  ASSERT_FALSE(ExtractFromSqlValue(SqlValue::Double(1.0), uint32).ok());
  ASSERT_FALSE(ExtractFromSqlValue(SqlValue::String("foo"), uint32).ok());
}

TEST(SqliteUtilsTest, ExtractFromSqlValueInt64) {
  std::optional<int64_t> int64;

  static constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
  static constexpr int64_t kMax = std::numeric_limits<int64_t>::max();

  ASSERT_TRUE(ExtractFromSqlValue(SqlValue::Long(1234), int64).ok());
  ASSERT_EQ(*int64, 1234);

  ASSERT_TRUE(ExtractFromSqlValue(SqlValue::Long(kMin), int64).ok());
  ASSERT_EQ(*int64, kMin);

  ASSERT_TRUE(ExtractFromSqlValue(SqlValue::Long(kMax), int64).ok());
  ASSERT_EQ(*int64, kMax);

  ASSERT_TRUE(ExtractFromSqlValue(SqlValue(), int64).ok());
  ASSERT_FALSE(int64.has_value());

  ASSERT_FALSE(ExtractFromSqlValue(SqlValue::Double(1.0), int64).ok());
  ASSERT_FALSE(ExtractFromSqlValue(SqlValue::String("foo"), int64).ok());
}

TEST(SqliteUtilsTest, ExtractFromSqlValueDouble) {
  std::optional<double> doub;

  static constexpr double kMin = std::numeric_limits<double>::min();
  static constexpr double kMax = std::numeric_limits<double>::max();

  ASSERT_TRUE(ExtractFromSqlValue(SqlValue::Double(1234.1), doub).ok());
  ASSERT_EQ(*doub, 1234.1);

  ASSERT_TRUE(ExtractFromSqlValue(SqlValue::Double(kMin), doub).ok());
  ASSERT_EQ(*doub, kMin);

  ASSERT_TRUE(ExtractFromSqlValue(SqlValue::Double(kMax), doub).ok());
  ASSERT_EQ(*doub, kMax);

  ASSERT_TRUE(ExtractFromSqlValue(SqlValue(), doub).ok());
  ASSERT_FALSE(doub.has_value());

  ASSERT_FALSE(ExtractFromSqlValue(SqlValue::Long(1234), doub).ok());
  ASSERT_FALSE(ExtractFromSqlValue(SqlValue::String("foo"), doub).ok());
}

TEST(SqliteUtilsTest, ExtractFromSqlValueString) {
  std::optional<const char*> string;

  ASSERT_TRUE(ExtractFromSqlValue(SqlValue::String("foo"), string).ok());
  ASSERT_STREQ(*string, "foo");

  ASSERT_TRUE(ExtractFromSqlValue(SqlValue(), string).ok());
  ASSERT_FALSE(string.has_value());

  ASSERT_FALSE(ExtractFromSqlValue(SqlValue::Long(1234), string).ok());
  ASSERT_FALSE(ExtractFromSqlValue(SqlValue::Double(123.1), string).ok());
}

}  // namespace
}  // namespace perfetto::trace_processor::sqlite::utils
