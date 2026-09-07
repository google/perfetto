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
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/perfetto_sql/lineage/type_mapping.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_connection.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::exec {
namespace {

using core::BitVector;
using core::Double;
using core::Int64;
using core::StorageType;
using core::String;
using core::exec::ColumnView;
using core::exec::kMaxBatchRows;
using core::exec::RowBatch;
using core::exec::RowCursor;
using core::exec::Variant;

// Drives a plan the way an executor does: creates the state, owns the batch.
class Execution {
 public:
  explicit Execution(const core::exec::Source& source)
      : source_(source), state_(source.MakeState()) {}

  RowBatch* Next() {
    return source_.GetData(batch_, *state_) ? &batch_ : nullptr;
  }
  void Rewind() { source_.Rewind(*state_); }
  base::Status status() const { return source_.status(*state_); }

 private:
  const core::exec::Source& source_;
  std::unique_ptr<core::exec::OperatorState> state_;
  RowBatch batch_;
};

std::vector<int64_t> ReadInts(const RowBatch& batch, uint32_t index) {
  std::vector<int64_t> out;
  for (const Variant& cell :
       core::exec::test::ReadColumn<Variant>(batch, index)) {
    out.push_back(cell.AsInt64());
  }
  return out;
}

struct TestColumn {
  std::string name;
  core::StorageType type;
};

TestColumn Typed(std::string name, core::StorageType type) {
  return {std::move(name), type};
}

class TestCatalog : public lineage::analysis::Catalog {
 public:
  void Add(std::string name, std::vector<TestColumn> columns) {
    dataframes_[std::move(name)] = std::move(columns);
  }

  std::optional<lineage::analysis::LeafRelation> FindLeafRelation(
      std::string_view name) const override {
    auto dataframe = dataframes_.find(std::string(name));
    if (dataframe == dataframes_.end()) {
      return std::nullopt;
    }
    lineage::analysis::LeafRelation relation;
    relation.name = name;
    for (const TestColumn& column : dataframe->second) {
      relation.columns.push_back(
          {column.name, lineage::ToAnalysisType(column.type)});
    }
    return relation;
  }

  std::optional<std::string> FindViewSql(std::string_view) const override {
    return std::nullopt;
  }

 private:
  std::map<std::string, std::vector<TestColumn>> dataframes_;
};

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

  base::StatusOr<std::unique_ptr<SqlScan>> Scan(
      const std::string& sql,
      const lineage::analysis::Catalog& catalog) {
    return SqlScan::Create(connection_.get(), SqlSource::FromExecuteQuery(sql),
                           &pool_, catalog);
  }

  // An empty catalog traces nothing, so every column is a variant.
  base::StatusOr<std::unique_ptr<SqlScan>> Scan(const std::string& sql) {
    return Scan(sql, empty_catalog_);
  }

  StringPool pool_;
  std::unique_ptr<SqliteConnection> connection_;
  TestCatalog empty_catalog_;
};

TEST_F(SqlScanTest, AQuerysColumnsAreKnownBeforeItsRows) {
  auto scan = Scan("SELECT 1 AS a, 'x' AS b");
  ASSERT_TRUE(scan.ok()) << scan.status().c_message();
  EXPECT_THAT((*scan)->column_names(), testing::ElementsAre("a", "b"));
}

TEST_F(SqlScanTest, AQuerysRowsArriveAsABatch) {
  auto scan = Scan(
      "SELECT a FROM ("
      "SELECT 2 AS ord, 20 AS a UNION ALL "
      "SELECT 3, 30 UNION ALL SELECT 1, 10) ORDER BY ord");
  ASSERT_TRUE(scan.ok()) << scan.status().c_message();
  Execution run(**scan);

  RowBatch* batch = run.Next();
  ASSERT_NE(batch, nullptr);
  EXPECT_EQ(batch->size(), 3u);
  EXPECT_THAT(ReadInts(*batch, 0), testing::ElementsAre(10, 20, 30));
  EXPECT_EQ(run.Next(), nullptr);
  EXPECT_TRUE(run.status().ok());
}

// One column holding three different types and a null, which SQLite allows.
TEST_F(SqlScanTest, OneColumnCanHoldMoreThanOneType) {
  auto scan = Scan(
      "SELECT value FROM ("
      "SELECT 1 AS ord, 7 AS value UNION ALL SELECT 2, 1.5 "
      "UNION ALL SELECT 3, 'hello' UNION ALL SELECT 4, NULL) ORDER BY ord");
  ASSERT_TRUE(scan.ok()) << scan.status().c_message();
  Execution run(**scan);

  RowBatch* batch = run.Next();
  ASSERT_NE(batch, nullptr);
  std::vector<Variant> cells = core::exec::test::ReadColumn<Variant>(*batch, 0);
  ASSERT_EQ(cells.size(), 4u);
  EXPECT_EQ(cells[0].AsInt64(), 7);
  EXPECT_EQ(cells[1].AsDouble(), 1.5);
  EXPECT_EQ(pool_.Get(cells[2].AsString()).ToStdString(), "hello");
  EXPECT_EQ(cells[3].type, Variant::Type::kNull);
  EXPECT_TRUE(run.status().ok());
}

// A declared type is not binding in SQLite, so it is not trusted here.
TEST_F(SqlScanTest, ADeclaredTypeIsNotBelieved) {
  Exec("CREATE TABLE t(i INTEGER)");
  Exec("INSERT INTO t VALUES(1), ('not a number')");
  auto scan = Scan("SELECT i FROM t");
  ASSERT_TRUE(scan.ok()) << scan.status().c_message();
  Execution run(**scan);

  RowBatch* batch = run.Next();
  ASSERT_NE(batch, nullptr);
  std::vector<Variant> cells = core::exec::test::ReadColumn<Variant>(*batch, 0);
  ASSERT_EQ(cells.size(), 2u);
  EXPECT_EQ(cells[0].AsInt64(), 1);
  EXPECT_EQ(pool_.Get(cells[1].AsString()).ToStdString(), "not a number");
  EXPECT_TRUE(run.status().ok());
}

TEST_F(SqlScanTest, AColumnWhichIsNeverAnythingIsAColumnOfNulls) {
  auto scan = Scan("SELECT NULL AS a UNION ALL SELECT NULL");
  ASSERT_TRUE(scan.ok()) << scan.status().c_message();
  Execution run(**scan);

  RowBatch* batch = run.Next();
  ASSERT_NE(batch, nullptr);
  for (const Variant& cell : core::exec::test::ReadColumn<Variant>(*batch, 0)) {
    EXPECT_EQ(cell.type, Variant::Type::kNull);
  }
}

TEST_F(SqlScanTest, MoreRowsThanFitInABatchArriveInSeveral) {
  Exec(
      "CREATE TABLE t AS WITH RECURSIVE r(x) AS ("
      "  SELECT 0 UNION ALL SELECT x + 1 FROM r WHERE x < 4999"
      ") SELECT x FROM r");
  auto scan = Scan("SELECT x FROM t ORDER BY x");
  ASSERT_TRUE(scan.ok()) << scan.status().c_message();
  Execution run(**scan);

  std::vector<int64_t> seen;
  std::vector<uint32_t> sizes;
  while (RowBatch* batch = run.Next()) {
    sizes.push_back(batch->size());
    for (int64_t value : ReadInts(*batch, 0)) {
      seen.push_back(value);
    }
  }
  ASSERT_TRUE(run.status().ok()) << run.status().c_message();
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
  Execution run(**scan);
  while (run.Next()) {
  }
  EXPECT_FALSE(run.status().ok());
}

TEST_F(SqlScanTest, ABlobIsReportedRatherThanCarried) {
  auto scan = Scan("SELECT x'0102' AS a");
  ASSERT_TRUE(scan.ok()) << scan.status().c_message();
  Execution run(**scan);

  EXPECT_EQ(run.Next(), nullptr);
  EXPECT_FALSE(run.status().ok());
  EXPECT_THAT(run.status().message(), testing::HasSubstr("blob"));
}

TEST_F(SqlScanTest, RewindClearsThePreviousExecutionError) {
  auto scan = Scan("SELECT x'0102' AS a");
  ASSERT_TRUE(scan.ok()) << scan.status().c_message();
  Execution run(**scan);
  ASSERT_EQ(run.Next(), nullptr);
  ASSERT_FALSE(run.status().ok());

  run.Rewind();
  EXPECT_TRUE(run.status().ok());
  EXPECT_EQ(run.Next(), nullptr);
  EXPECT_FALSE(run.status().ok());
}

TEST_F(SqlScanTest, EachExecutionValidatesItsResultShape) {
  Exec("CREATE TABLE t(a INTEGER)");
  Exec("INSERT INTO t VALUES(1)");
  auto scan = Scan("SELECT * FROM t");
  ASSERT_TRUE(scan.ok()) << scan.status().c_message();
  Exec("ALTER TABLE t ADD COLUMN b TEXT");

  Execution run(**scan);
  EXPECT_EQ(run.Next(), nullptr);
  EXPECT_FALSE(run.status().ok());
  EXPECT_THAT(run.status().message(), testing::HasSubstr("shape changed"));
}

TEST_F(SqlScanTest, AScanCanBeRunAgain) {
  auto scan = Scan("SELECT 1 AS a UNION ALL SELECT 2");
  ASSERT_TRUE(scan.ok()) << scan.status().c_message();
  Execution run(**scan);

  auto drain = [&] {
    std::vector<int64_t> out;
    while (RowBatch* batch = run.Next()) {
      for (int64_t value : ReadInts(*batch, 0)) {
        out.push_back(value);
      }
    }
    return out;
  };
  std::vector<int64_t> first = drain();
  run.Rewind();
  EXPECT_EQ(drain(), first);
}

TEST_F(SqlScanTest, AQueryReachesARowCursor) {
  auto scan = Scan(
      "SELECT a FROM (SELECT 2 AS ord, 6 AS a UNION ALL "
      "SELECT 3, 7 UNION ALL SELECT 1, 5) ORDER BY ord");
  ASSERT_TRUE(scan.ok()) << scan.status().c_message();

  RowCursor cursor(**scan);
  std::vector<int64_t> values;
  for (bool more = cursor.Open(); more; more = cursor.Next()) {
    values.push_back(cursor.Value<Variant>(0).AsInt64());
  }
  EXPECT_THAT(values, testing::ElementsAre(5, 6, 7));
}

// A column which can be traced back to a dataframe needs neither a variant nor
// an assertion: it comes out flat.
TEST_F(SqlScanTest, AColumnFollowedBackToADataframeComesOutFlat) {
  Exec("CREATE TABLE df(id INTEGER, name TEXT)");
  Exec("INSERT INTO df VALUES(7, 'hello'), (8, NULL)");
  TestCatalog catalog;
  catalog.Add("df", {Typed("id", core::StorageType{core::Int64{}}),
                     Typed("name", core::StorageType{core::String{}})});

  auto scan = Scan("SELECT id, name FROM df ORDER BY id", catalog);
  ASSERT_TRUE(scan.ok()) << scan.status().c_message();
  ASSERT_TRUE((*scan)->column_type(0).has_value());
  EXPECT_TRUE((*scan)->column_type(0)->Is<core::Int64>());
  EXPECT_TRUE((*scan)->column_type(1)->Is<core::String>());

  Execution run(**scan);
  RowBatch* batch = run.Next();
  ASSERT_NE(batch, nullptr);
  EXPECT_EQ(batch->column(0).kind(), ColumnView::Kind::kFlat);
  const auto* ids = static_cast<const int64_t*>(batch->column(0).data());
  EXPECT_EQ(ids[0], 7);
  EXPECT_EQ(ids[1], 8);
  const BitVector* validity = batch->column(1).validity();
  ASSERT_NE(validity, nullptr);
  EXPECT_TRUE(validity->is_set(0));
  EXPECT_FALSE(validity->is_set(1));
  std::vector<StringPool::Id> names =
      core::exec::test::ReadColumn<StringPool::Id>(*batch, 1);
  EXPECT_EQ(pool_.Get(names[0]).ToStdString(), "hello");
}

TEST_F(SqlScanTest, MixedNumericCompoundResultsStayVariants) {
  Exec("CREATE TABLE ints(value INTEGER)");
  Exec("INSERT INTO ints VALUES(7)");
  Exec("CREATE TABLE doubles(value REAL)");
  Exec("INSERT INTO doubles VALUES(1.5)");
  TestCatalog catalog;
  catalog.Add("ints", {Typed("value", StorageType{Int64{}})});
  catalog.Add("doubles", {Typed("value", StorageType{Double{}})});

  auto scan = Scan(
      "SELECT value FROM ("
      "SELECT 1 AS ord, value FROM ints UNION ALL "
      "SELECT 2, value FROM doubles) ORDER BY ord",
      catalog);
  ASSERT_TRUE(scan.ok()) << scan.status().c_message();
  EXPECT_FALSE((*scan)->column_type(0).has_value());
  Execution run(**scan);
  RowBatch* batch = run.Next();
  ASSERT_NE(batch, nullptr);
  std::vector<Variant> values =
      core::exec::test::ReadColumn<Variant>(*batch, 0);
  ASSERT_EQ(values.size(), 2u);
  EXPECT_EQ(values[0].AsInt64(), 7);
  EXPECT_EQ(values[1].AsDouble(), 1.5);
}

// An expression cannot be traced back, so it stays a variant even when the
// column beside it does not.
TEST_F(SqlScanTest, OnlyTheColumnsWhichCanBeFollowedComeOutFlat) {
  Exec("CREATE TABLE df(id INTEGER)");
  Exec("INSERT INTO df VALUES(7)");
  TestCatalog catalog;
  catalog.Add("df", {Typed("id", core::StorageType{core::Int64{}})});

  auto scan = Scan("SELECT id, id * 2 AS doubled FROM df", catalog);
  ASSERT_TRUE(scan.ok()) << scan.status().c_message();
  EXPECT_TRUE((*scan)->column_type(0).has_value());
  EXPECT_FALSE((*scan)->column_type(1).has_value());

  Execution run(**scan);
  RowBatch* batch = run.Next();
  ASSERT_NE(batch, nullptr);
  EXPECT_EQ(batch->column(0).kind(), ColumnView::Kind::kFlat);
  EXPECT_EQ(batch->column(1).kind(), ColumnView::Kind::kVariant);
}

// Without a catalog nothing can be traced back.
TEST_F(SqlScanTest, WithoutACatalogEveryColumnIsAVariant) {
  Exec("CREATE TABLE df(id INTEGER)");
  auto scan = Scan("SELECT id FROM df");
  ASSERT_TRUE(scan.ok()) << scan.status().c_message();
  EXPECT_FALSE((*scan)->column_type(0).has_value());
}

// A flat column's storage is readable at every row, so a reader which sums it
// without checking validity gets zero rather than a value left over from the
// previous batch.
TEST_F(SqlScanTest, ANullSlotOfAFlatColumnHoldsZero) {
  Exec("CREATE TABLE df(id INTEGER)");
  Exec("INSERT INTO df VALUES(7), (NULL)");
  TestCatalog catalog;
  catalog.Add("df", {Typed("id", core::StorageType{core::Int64{}})});

  auto scan = Scan("SELECT id FROM df ORDER BY id IS NULL", catalog);
  ASSERT_TRUE(scan.ok()) << scan.status().c_message();
  Execution run(**scan);
  RowBatch* batch = run.Next();
  ASSERT_NE(batch, nullptr);
  ASSERT_EQ(batch->size(), 2u);

  const auto* ids = static_cast<const int64_t*>(batch->column(0).data());
  EXPECT_EQ(ids[0], 7);
  EXPECT_EQ(ids[1], 0);
  EXPECT_FALSE(batch->column(0).validity()->is_set(1));
}

}  // namespace
}  // namespace perfetto::trace_processor::exec
