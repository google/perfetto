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

#include "src/trace_processor/perfetto_sql/exec/sql_scan.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_connection.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::exec {
namespace {

using core::Double;
using core::Int64;
using core::StorageType;
using core::String;
using core::exec::ColumnView;
using core::exec::kMaxBatchRows;
using core::exec::RowBatch;
using core::exec::RowCursor;

// Reads column `index` of `batch` as `T`, with null for the rows which hold
// nothing.
template <typename T>
std::vector<std::optional<T>> Read(const RowBatch& batch, uint32_t index) {
  const ColumnView& column = batch.column(index);
  const auto* data = static_cast<const T*>(column.data());
  std::vector<std::optional<T>> out;
  for (uint32_t i = 0; i < batch.size(); ++i) {
    uint32_t row = column.selection().GetIndex(i);
    if (column.validity() && !column.validity()->is_set(row)) {
      out.emplace_back();
      continue;
    }
    out.push_back(data[row]);
  }
  return out;
}

class SqlScanTest : public ::testing::Test {
 protected:
  SqlScanTest()
      : connection_(SqliteConnection::CreateConnectionToNewDatabase()) {}

  void Exec(const std::string& sql) {
    auto statement =
        connection_->PrepareStatement(SqlSource::FromExecuteQuery(sql));
    ASSERT_TRUE(statement.status().ok()) << statement.status().c_message();
    while (statement.Step()) {
    }
    ASSERT_TRUE(statement.status().ok()) << statement.status().c_message();
  }

  base::StatusOr<std::unique_ptr<SqlScan>> Scan(const std::string& sql) {
    return SqlScan::Create(connection_.get(), SqlSource::FromExecuteQuery(sql),
                           &pool_);
  }

  StringPool pool_;
  std::unique_ptr<SqliteConnection> connection_;
};

TEST_F(SqlScanTest, AQuerysColumnsAreKnownBeforeItsRows) {
  auto scan = Scan("SELECT 1 AS a, 'x' AS b");
  ASSERT_TRUE(scan.ok()) << scan.status().c_message();
  EXPECT_THAT((*scan)->column_names(), testing::ElementsAre("a", "b"));
}

TEST_F(SqlScanTest, AQuerysRowsArriveAsABatch) {
  auto scan = Scan("SELECT 10 AS a UNION ALL SELECT 20 UNION ALL SELECT 30");
  ASSERT_TRUE(scan.ok()) << scan.status().c_message();

  RowBatch* batch = (*scan)->Next();
  ASSERT_NE(batch, nullptr);
  EXPECT_EQ(batch->size(), 3u);
  EXPECT_TRUE(batch->column(0).type().Is<Int64>());
  EXPECT_THAT(Read<int64_t>(*batch, 0),
              testing::ElementsAre(std::optional<int64_t>(10),
                                   std::optional<int64_t>(20),
                                   std::optional<int64_t>(30)));
  EXPECT_EQ((*scan)->Next(), nullptr);
  EXPECT_TRUE((*scan)->status().ok());
}

TEST_F(SqlScanTest, EachOfTheTypesAPipelineCarriesComesThrough) {
  auto scan = Scan("SELECT 7 AS i, 1.5 AS d, 'hello' AS s");
  ASSERT_TRUE(scan.ok()) << scan.status().c_message();

  RowBatch* batch = (*scan)->Next();
  ASSERT_NE(batch, nullptr);
  ASSERT_EQ(batch->size(), 1u);
  EXPECT_TRUE(batch->column(0).type().Is<Int64>());
  EXPECT_TRUE(batch->column(1).type().Is<Double>());
  EXPECT_TRUE(batch->column(2).type().Is<String>());
  EXPECT_EQ(Read<int64_t>(*batch, 0)[0], std::optional<int64_t>(7));
  EXPECT_EQ(Read<double>(*batch, 1)[0], std::optional<double>(1.5));

  auto strings = Read<StringPool::Id>(*batch, 2);
  ASSERT_TRUE(strings[0].has_value());
  EXPECT_EQ(pool_.Get(*strings[0]).ToStdString(), "hello");
}

TEST_F(SqlScanTest, ANullIsARowWhichHoldsNothing) {
  auto scan = Scan("SELECT 1 AS a UNION ALL SELECT NULL UNION ALL SELECT 3");
  ASSERT_TRUE(scan.ok()) << scan.status().c_message();

  RowBatch* batch = (*scan)->Next();
  ASSERT_NE(batch, nullptr);
  EXPECT_THAT(Read<int64_t>(*batch, 0),
              testing::ElementsAre(std::optional<int64_t>(1), std::nullopt,
                                   std::optional<int64_t>(3)));
}

TEST_F(SqlScanTest, AColumnWhichIsNeverAnythingIsAColumnOfNulls) {
  auto scan = Scan("SELECT NULL AS a UNION ALL SELECT NULL");
  ASSERT_TRUE(scan.ok()) << scan.status().c_message();

  RowBatch* batch = (*scan)->Next();
  ASSERT_NE(batch, nullptr);
  EXPECT_THAT(Read<int64_t>(*batch, 0),
              testing::ElementsAre(std::nullopt, std::nullopt));
}

TEST_F(SqlScanTest, MoreRowsThanFitInABatchArriveInSeveral) {
  Exec(
      "CREATE TABLE t AS WITH RECURSIVE r(x) AS ("
      "  SELECT 0 UNION ALL SELECT x + 1 FROM r WHERE x < 4999"
      ") SELECT x FROM r");
  auto scan = Scan("SELECT x FROM t ORDER BY x");
  ASSERT_TRUE(scan.ok()) << scan.status().c_message();

  std::vector<int64_t> seen;
  std::vector<uint32_t> sizes;
  while (RowBatch* batch = (*scan)->Next()) {
    sizes.push_back(batch->size());
    for (const std::optional<int64_t>& value : Read<int64_t>(*batch, 0)) {
      ASSERT_TRUE(value.has_value());
      seen.push_back(*value);
    }
  }
  ASSERT_TRUE((*scan)->status().ok()) << (*scan)->status().c_message();
  EXPECT_THAT(sizes, testing::ElementsAre(kMaxBatchRows, kMaxBatchRows, 904u));
  ASSERT_EQ(seen.size(), 5000u);
  for (uint32_t i = 0; i < seen.size(); ++i) {
    ASSERT_EQ(seen[i], int64_t{i});
  }
}

TEST_F(SqlScanTest, AQueryWhichDoesNotRunIsReported) {
  auto scan = Scan("SELECT * FROM not_a_table");
  EXPECT_FALSE(scan.ok());
}

TEST_F(SqlScanTest, AQueryWhichFailsPartWayThroughIsReported) {
  auto scan =
      Scan("SELECT 1 AS a UNION ALL SELECT abs(-9223372036854775807 - 1)");
  ASSERT_TRUE(scan.ok()) << scan.status().c_message();
  // A query which dies half way must not look like one which simply ended.
  while ((*scan)->Next()) {
  }
  EXPECT_FALSE((*scan)->status().ok());
}

TEST_F(SqlScanTest, AColumnWhichChangesTypeIsReported) {
  auto scan = Scan("SELECT 1 AS a UNION ALL SELECT 'two'");
  ASSERT_TRUE(scan.ok()) << scan.status().c_message();

  EXPECT_EQ((*scan)->Next(), nullptr);
  EXPECT_FALSE((*scan)->status().ok());
  EXPECT_THAT((*scan)->status().message(), testing::HasSubstr("'a'"));
}

TEST_F(SqlScanTest, ABlobIsReportedRatherThanCarried) {
  auto scan = Scan("SELECT x'0102' AS a");
  ASSERT_TRUE(scan.ok()) << scan.status().c_message();

  EXPECT_EQ((*scan)->Next(), nullptr);
  EXPECT_FALSE((*scan)->status().ok());
  EXPECT_THAT((*scan)->status().message(), testing::HasSubstr("blob"));
}

TEST_F(SqlScanTest, AScanCanBeRunAgain) {
  auto scan = Scan("SELECT 1 AS a UNION ALL SELECT 2");
  ASSERT_TRUE(scan.ok()) << scan.status().c_message();

  auto run = [&] {
    std::vector<std::optional<int64_t>> out;
    while (RowBatch* batch = (*scan)->Next()) {
      for (const std::optional<int64_t>& value : Read<int64_t>(*batch, 0)) {
        out.push_back(value);
      }
    }
    return out;
  };
  std::vector<std::optional<int64_t>> first = run();
  (*scan)->Reset();
  EXPECT_EQ(run(), first);
}

// The point of being a Source: a query reaches the thing which reads rows
// without either end knowing about the other.
TEST_F(SqlScanTest, AQueryReachesARowCursor) {
  auto scan = Scan("SELECT 5 AS a UNION ALL SELECT 6 UNION ALL SELECT 7");
  ASSERT_TRUE(scan.ok()) << scan.status().c_message();

  RowCursor cursor(**scan);
  std::vector<uint32_t> rows;
  for (bool more = cursor.Open(); more; more = cursor.Next()) {
    rows.push_back(cursor.row(0));
  }
  EXPECT_THAT(rows, testing::ElementsAre(0u, 1u, 2u));
}

}  // namespace
}  // namespace perfetto::trace_processor::exec
