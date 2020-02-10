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

DbSqliteTable::TableOutline CreateOutline(uint32_t row_count) {
  DbSqliteTable::TableOutline outline;
  outline.row_count = row_count;

  outline.columns.push_back({true /* is_id */, true /* is_sorted */});
  outline.columns.push_back({false /* is_id */, false /* is_sorted */});
  outline.columns.push_back({false /* is_id */, true /* is_sorted */});
  outline.columns.push_back({false /* is_id */, false /* is_sorted */});
  outline.columns.push_back({false /* is_id */, false /* is_sorted */});

  return outline;
}

TEST(DbSqliteTable, IdEqCheaperThanOtherEq) {
  auto outline = CreateOutline(1234);

  QueryConstraints id_eq;
  id_eq.AddConstraint(0u, SQLITE_INDEX_CONSTRAINT_EQ, 0u);

  auto id_cost = DbSqliteTable::EstimateCost(outline, id_eq);

  QueryConstraints a_eq;
  a_eq.AddConstraint(1u, SQLITE_INDEX_CONSTRAINT_EQ, 1u);

  auto a_cost = DbSqliteTable::EstimateCost(outline, a_eq);

  ASSERT_LT(id_cost.cost, a_cost.cost);
  ASSERT_LT(id_cost.rows, a_cost.rows);
}

TEST(DbSqliteTable, IdEqCheaperThatOtherConstraint) {
  auto outline = CreateOutline(1234);

  QueryConstraints id_eq;
  id_eq.AddConstraint(0u, SQLITE_INDEX_CONSTRAINT_EQ, 0u);

  auto id_cost = DbSqliteTable::EstimateCost(outline, id_eq);

  QueryConstraints a_eq;
  a_eq.AddConstraint(1u, SQLITE_INDEX_CONSTRAINT_LT, 1u);

  auto a_cost = DbSqliteTable::EstimateCost(outline, a_eq);

  ASSERT_LT(id_cost.cost, a_cost.cost);
  ASSERT_LT(id_cost.rows, a_cost.rows);
}

TEST(DbSqliteTable, SingleEqCheaperThanMultipleConstraint) {
  auto outline = CreateOutline(1234);

  QueryConstraints single_eq;
  single_eq.AddConstraint(1u, SQLITE_INDEX_CONSTRAINT_EQ, 0u);

  auto single_cost = DbSqliteTable::EstimateCost(outline, single_eq);

  QueryConstraints multi_eq;
  multi_eq.AddConstraint(1u, SQLITE_INDEX_CONSTRAINT_EQ, 0u);
  multi_eq.AddConstraint(2u, SQLITE_INDEX_CONSTRAINT_EQ, 1u);

  auto multi_cost = DbSqliteTable::EstimateCost(outline, multi_eq);

  // The cost of the single filter should be cheaper (because of our special
  // handling of single equality). But the number of rows should be greater.
  ASSERT_LT(single_cost.cost, multi_cost.cost);
  ASSERT_GT(single_cost.rows, multi_cost.rows);
}

TEST(DbSqliteTable, MultiSortedEqCheaperThanMultiUnsortedEq) {
  auto outline = CreateOutline(1234);

  QueryConstraints sorted_eq;
  sorted_eq.AddConstraint(2u, SQLITE_INDEX_CONSTRAINT_EQ, 0u);
  sorted_eq.AddConstraint(3u, SQLITE_INDEX_CONSTRAINT_EQ, 0u);

  auto sorted_cost = DbSqliteTable::EstimateCost(outline, sorted_eq);

  QueryConstraints unsorted_eq;
  unsorted_eq.AddConstraint(3u, SQLITE_INDEX_CONSTRAINT_EQ, 0u);
  unsorted_eq.AddConstraint(4u, SQLITE_INDEX_CONSTRAINT_EQ, 0u);

  auto unsorted_cost = DbSqliteTable::EstimateCost(outline, unsorted_eq);

  // The number of rows should be the same but the cost of the sorted
  // query should be less.
  ASSERT_LT(sorted_cost.cost, unsorted_cost.cost);
  ASSERT_EQ(sorted_cost.rows, unsorted_cost.rows);
}

TEST(DbSqliteTable, EmptyTableCosting) {
  auto outline = CreateOutline(0u);

  QueryConstraints id_eq;
  id_eq.AddConstraint(0u, SQLITE_INDEX_CONSTRAINT_EQ, 0u);

  auto id_cost = DbSqliteTable::EstimateCost(outline, id_eq);

  QueryConstraints a_eq;
  a_eq.AddConstraint(1u, SQLITE_INDEX_CONSTRAINT_LT, 1u);

  auto a_cost = DbSqliteTable::EstimateCost(outline, a_eq);

  ASSERT_DOUBLE_EQ(id_cost.cost, a_cost.cost);
  ASSERT_EQ(id_cost.rows, a_cost.rows);
}

TEST(DbSqliteTable, OrderByOnSortedCheaper) {
  auto outline = CreateOutline(1234);

  QueryConstraints a_qc;
  a_qc.AddOrderBy(1u, false);

  auto a_cost = DbSqliteTable::EstimateCost(outline, a_qc);

  // On an ordered column, the constraint for sorting would get pruned so
  // we would end up with an empty constraint set.
  QueryConstraints sorted_qc;
  auto sorted_cost = DbSqliteTable::EstimateCost(outline, sorted_qc);

  ASSERT_LT(sorted_cost.cost, a_cost.cost);
  ASSERT_EQ(sorted_cost.rows, a_cost.rows);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
