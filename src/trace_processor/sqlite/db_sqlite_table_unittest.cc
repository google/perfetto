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
    columns_.emplace_back(
        Column("sorted", &sorted_, Column::Flag::kSorted, this, 2u, 0u));
  }

 private:
  StringPool pool_;
  SparseVector<uint32_t> a_;
  SparseVector<uint32_t> sorted_;
};

TEST(DbSqliteTable, IdEqCheaperThanOtherEq) {
  TestTable table(1234);

  QueryConstraints id_eq;
  id_eq.AddConstraint(0u, SQLITE_INDEX_CONSTRAINT_EQ, 0u);

  auto id_cost = DbSqliteTable::EstimateCost(table, id_eq);

  QueryConstraints a_eq;
  a_eq.AddConstraint(1u, SQLITE_INDEX_CONSTRAINT_EQ, 1u);

  auto a_cost = DbSqliteTable::EstimateCost(table, a_eq);

  ASSERT_LT(id_cost.cost, a_cost.cost);
  ASSERT_LT(id_cost.rows, a_cost.rows);
}

TEST(DbSqliteTable, IdEqCheaperThatOtherConstraint) {
  TestTable table(1234);

  QueryConstraints id_eq;
  id_eq.AddConstraint(0u, SQLITE_INDEX_CONSTRAINT_EQ, 0u);

  auto id_cost = DbSqliteTable::EstimateCost(table, id_eq);

  QueryConstraints a_eq;
  a_eq.AddConstraint(1u, SQLITE_INDEX_CONSTRAINT_LT, 1u);

  auto a_cost = DbSqliteTable::EstimateCost(table, a_eq);

  ASSERT_LT(id_cost.cost, a_cost.cost);
  ASSERT_LT(id_cost.rows, a_cost.rows);
}

TEST(DbSqliteTable, SingleEqCheaperThanMultipleConstraint) {
  TestTable table(1234);

  QueryConstraints single_eq;
  single_eq.AddConstraint(1u, SQLITE_INDEX_CONSTRAINT_EQ, 0u);

  auto single_cost = DbSqliteTable::EstimateCost(table, single_eq);

  QueryConstraints multi_eq;
  multi_eq.AddConstraint(1u, SQLITE_INDEX_CONSTRAINT_EQ, 0u);
  multi_eq.AddConstraint(2u, SQLITE_INDEX_CONSTRAINT_EQ, 1u);

  auto multi_cost = DbSqliteTable::EstimateCost(table, multi_eq);

  // The cost of the single filter should be cheaper (because of our special
  // handling of single equality). But the number of rows should be greater.
  ASSERT_LT(single_cost.cost, multi_cost.cost);
  ASSERT_GT(single_cost.rows, multi_cost.rows);
}

TEST(DbSqliteTable, EmptyTableCosting) {
  TestTable table(0u);

  QueryConstraints id_eq;
  id_eq.AddConstraint(0u, SQLITE_INDEX_CONSTRAINT_EQ, 0u);

  auto id_cost = DbSqliteTable::EstimateCost(table, id_eq);

  QueryConstraints a_eq;
  a_eq.AddConstraint(1u, SQLITE_INDEX_CONSTRAINT_LT, 1u);

  auto a_cost = DbSqliteTable::EstimateCost(table, a_eq);

  ASSERT_DOUBLE_EQ(id_cost.cost, a_cost.cost);
  ASSERT_EQ(id_cost.rows, a_cost.rows);
}

TEST(DbSqliteTable, OrderByOnSortedCheaper) {
  TestTable table(1234);

  QueryConstraints a_qc;
  a_qc.AddOrderBy(1u, false);

  auto a_cost = DbSqliteTable::EstimateCost(table, a_qc);

  // On an ordered column, the constraint for sorting would get pruned so
  // we would end up with an empty constraint set.
  QueryConstraints sorted_qc;
  auto sorted_cost = DbSqliteTable::EstimateCost(table, sorted_qc);

  ASSERT_LT(sorted_cost.cost, a_cost.cost);
  ASSERT_EQ(sorted_cost.rows, a_cost.rows);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
