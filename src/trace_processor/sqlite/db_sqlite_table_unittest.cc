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

#include "src/trace_processor/sqlite/db_sqlite_table.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

class TestTable : public Table {
 public:
  TestTable(uint32_t size) : Table(&pool_, nullptr) {
    row_maps_.emplace_back(RowMap(0, size));
    size_ = size;

    columns_.emplace_back(Column::IdColumn(this, 0u, 0u));
    columns_.emplace_back(
        Column("a", &a_, Column::Flag::kNoFlag, this, 1u, 0u));
  }

 private:
  StringPool pool_;
  SparseVector<uint32_t> a_;
};

TEST(DbSqliteTableTest, EstimateCostEmpty) {
  TestTable table(0u);

  auto cost = DbSqliteTable::EstimateCost(table, QueryConstraints());
  ASSERT_EQ(cost.rows, 0u);

  // The cost should be 1000 (fixed cost).
  ASSERT_DOUBLE_EQ(cost.cost, 1000.0);
}

TEST(DbSqliteTableTest, EstimateCostSmoke) {
  TestTable table(1234u);

  auto cost = DbSqliteTable::EstimateCost(table, QueryConstraints());
  ASSERT_EQ(cost.rows, 1234u);

  // The cost should be 1000 (fixed cost) + 2468 (iteration cost).
  ASSERT_DOUBLE_EQ(cost.cost, 3468);
}

TEST(DbSqliteTableTest, EstimateCostFilterId) {
  TestTable table(1234u);

  QueryConstraints qc;
  qc.AddConstraint(0u, SQLITE_INDEX_CONSTRAINT_EQ, 0u);

  auto cost = DbSqliteTable::EstimateCost(table, qc);
  ASSERT_EQ(cost.rows, 1u);

  // The cost should be 1000 (fixed cost) + 100 (filter cost) + 2 (iteration
  // cost).
  ASSERT_DOUBLE_EQ(cost.cost, 1102);
}

TEST(DbSqliteTableTest, EstimateCostEqualityConstraint) {
  TestTable table(1234u);

  QueryConstraints qc;
  qc.AddConstraint(1u, SQLITE_INDEX_CONSTRAINT_EQ, 0u);

  auto cost = DbSqliteTable::EstimateCost(table, qc);
  ASSERT_EQ(cost.rows, 120u);

  // The cost should be 1000 (fixed cost) + 240.332 (filter cost) + 240
  // (iteration cost).
  ASSERT_DOUBLE_EQ(round(cost.cost), 1480);
}

TEST(DbSqliteTableTest, EstimateCostSort) {
  TestTable table(1234u);

  QueryConstraints qc;
  qc.AddOrderBy(1u, false);

  auto cost = DbSqliteTable::EstimateCost(table, qc);
  ASSERT_EQ(cost.rows, 1234u);

  // The cost should be 1000 (fixed cost) + 12672.102 (sort cost) + 2468
  // (iteration cost).
  ASSERT_DOUBLE_EQ(round(cost.cost), 16140);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
