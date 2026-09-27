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

#include "src/trace_processor/perfetto_sql/exec/collected_rows.h"

#include <sqlite3.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/schema.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/exec/variant.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::exec {
namespace {

using core::StorageType;
using core::exec::kMaxBatchRows;
using core::exec::RowBatch;
using core::exec::Variant;
using core::exec::test::ReadNullableColumn;
using testing::ElementsAre;
using testing::HasSubstr;

class CollectedRowsTest : public testing::Test {
 protected:
  CollectedRowsTest() { sqlite3_open(":memory:", &db_); }
  ~CollectedRowsTest() override { sqlite3_close(db_); }

  // Appends every row of `sql` to `rows`, as SQLite would hand them over.
  base::Status Collect(const std::string& sql, CollectedRows& rows) {
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr),
              SQLITE_OK);
    base::Status status = base::OkStatus();
    while (status.ok() && sqlite3_step(stmt) == SQLITE_ROW) {
      std::vector<sqlite3_value*> values;
      for (int i = 0; i < sqlite3_column_count(stmt); ++i) {
        values.push_back(sqlite3_column_value(stmt, i));
      }
      status = rows.Append(values.data());
    }
    sqlite3_finalize(stmt);
    return status;
  }

  StringPool pool_;
  sqlite3* db_ = nullptr;
};

TEST_F(CollectedRowsTest, TypedColumnsHoldValuesOrNulls) {
  CollectedRows rows(
      {{"a", StorageType{core::Int64{}}}, {"b", StorageType{core::String{}}}},
      &pool_);
  ASSERT_TRUE(Collect("SELECT 1, 'x' UNION ALL SELECT NULL, NULL", rows).ok());
  ASSERT_EQ(rows.batch_count(), 1u);
  RowBatch batch;
  rows.View(0, batch);
  EXPECT_THAT(ReadNullableColumn<int64_t>(batch, 0),
              ElementsAre(1, std::nullopt));
  std::vector<std::optional<StringPool::Id>> b =
      ReadNullableColumn<StringPool::Id>(batch, 1);
  ASSERT_EQ(b.size(), 2u);
  EXPECT_EQ(pool_.Get(*b[0]).ToStdString(), "x");
  EXPECT_FALSE(b[1].has_value());
}

TEST_F(CollectedRowsTest, UntypedColumnsCarryTypesPerRow) {
  CollectedRows rows({{"v", std::nullopt}}, &pool_);
  ASSERT_TRUE(Collect("SELECT 1 UNION ALL SELECT 2.5 UNION ALL SELECT 'x' "
                      "UNION ALL SELECT NULL",
                      rows)
                  .ok());
  RowBatch batch;
  rows.View(0, batch);
  const auto* values = static_cast<const Variant*>(batch.column(0).data());
  EXPECT_EQ(values[0].AsInt64(), 1);
  EXPECT_EQ(values[1].AsDouble(), 2.5);
  EXPECT_EQ(pool_.Get(values[2].AsString()).ToStdString(), "x");
  EXPECT_EQ(values[3].type, Variant::Type::kNull);
}

TEST_F(CollectedRowsTest, BlobsCannotBeCarried) {
  CollectedRows rows({{"v", std::nullopt}}, &pool_);
  EXPECT_THAT(Collect("SELECT X'00'", rows).message(),
              HasSubstr("column 'v' holds a blob"));
}

TEST_F(CollectedRowsTest, TracedTypesMustHold) {
  CollectedRows rows({{"a", StorageType{core::Int64{}}}}, &pool_);
  EXPECT_THAT(Collect("SELECT 'x'", rows).message(),
              HasSubstr("column 'a' does not hold what it was traced back to"));
}

TEST_F(CollectedRowsTest, RowsFillBatches) {
  CollectedRows rows({{"v", StorageType{core::Int64{}}}}, &pool_);
  std::string sql =
      "WITH RECURSIVE n(v) AS (SELECT 0 UNION ALL SELECT v + 1 "
      "FROM n WHERE v + 1 < " +
      std::to_string(kMaxBatchRows + 1) + ") SELECT v FROM n";
  ASSERT_TRUE(Collect(sql, rows).ok());
  ASSERT_EQ(rows.batch_count(), 2u);
  RowBatch batch;
  rows.View(1, batch);
  EXPECT_THAT(ReadNullableColumn<int64_t>(batch, 0),
              ElementsAre(kMaxBatchRows));
}

TEST_F(CollectedRowsTest, ScansReplayTheirInput) {
  auto rows = std::make_shared<CollectedRows>(
      core::Schema{{"v", StorageType{core::Int64{}}}}, &pool_);
  ASSERT_TRUE(Collect("SELECT 1 UNION ALL SELECT 2", *rows).ok());
  CollectedRowsScan::Inputs inputs{rows};
  CollectedRowsScan scan(inputs, 0);
  for (int read = 0; read < 2; ++read) {
    core::exec::RowCursor cursor(scan);
    ASSERT_TRUE(cursor.Open());
    EXPECT_THAT(ReadNullableColumn<int64_t>(cursor.batch(), 0),
                ElementsAre(1, 2));
    uint32_t count = 1;
    while (cursor.Next()) {
      ++count;
    }
    EXPECT_EQ(count, 2u);
  }
}

}  // namespace
}  // namespace perfetto::trace_processor::exec
