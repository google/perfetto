
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

#include <sqlite3.h>
#include <array>
#include <cstdint>

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/db/table.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

Table::Schema CreateSchema() {
  Table::Schema schema;
  schema.columns.push_back({"id", SqlValue::Type::kLong, true /* is_id */,
                            true /* is_sorted */, false /* is_hidden */,
                            false /* is_set_id */});
  schema.columns.push_back({"type", SqlValue::Type::kLong, false /* is_id */,
                            false /* is_sorted */, false /* is_hidden */,
                            false /* is_set_id */});
  schema.columns.push_back({"test1", SqlValue::Type::kLong, false /* is_id */,
                            true /* is_sorted */, false /* is_hidden */,
                            false /* is_set_id */});
  schema.columns.push_back({"test2", SqlValue::Type::kLong, false /* is_id */,
                            false /* is_sorted */, false /* is_hidden */,
                            false /* is_set_id */});
  schema.columns.push_back({"test3", SqlValue::Type::kLong, false /* is_id */,
                            false /* is_sorted */, false /* is_hidden */,
                            false /* is_set_id */});
  return schema;
}

sqlite3_index_info::sqlite3_index_constraint CreateConstraint(int col,
                                                              uint8_t op) {
  return {col, op, true, 0};
}

sqlite3_index_info::sqlite3_index_constraint_usage CreateUsage() {
  return {1, true};
}

sqlite3_index_info CreateCsIndexInfo(
    int cs_count,
    sqlite3_index_info::sqlite3_index_constraint* c,
    sqlite3_index_info::sqlite3_index_constraint_usage* u) {
  return {cs_count, c, 0, nullptr, u, 0, nullptr, false, 0, 0, 0, 0, 0};
}

sqlite3_index_info CreateObIndexInfo(
    int ob_count,
    sqlite3_index_info::sqlite3_index_orderby* o) {
  return {0, nullptr, ob_count, o, nullptr, 0, nullptr, false, 0, 0, 0, 0, 0};
}

TEST(DbSqliteModule, IdEqCheaperThanOtherEq) {
  auto schema = CreateSchema();
  constexpr uint32_t kRowCount = 1234;

  auto c = CreateConstraint(0, SQLITE_INDEX_CONSTRAINT_EQ);
  auto u = CreateUsage();
  auto info = CreateCsIndexInfo(1, &c, &u);

  auto id_cost =
      DbSqliteModule::EstimateCost(schema, kRowCount, &info, {0u}, {});

  c.iColumn = 1;
  auto a_cost =
      DbSqliteModule::EstimateCost(schema, kRowCount, &info, {0u}, {});

  ASSERT_LT(id_cost.cost, a_cost.cost);
  ASSERT_LT(id_cost.rows, a_cost.rows);
}

TEST(DbSqliteModule, IdEqCheaperThatOtherConstraint) {
  auto schema = CreateSchema();
  constexpr uint32_t kRowCount = 1234;

  auto c = CreateConstraint(0, SQLITE_INDEX_CONSTRAINT_EQ);
  auto u = CreateUsage();
  auto info = CreateCsIndexInfo(1, &c, &u);

  auto id_cost =
      DbSqliteModule::EstimateCost(schema, kRowCount, &info, {0u}, {});

  c.iColumn = 1;
  c.op = SQLITE_INDEX_CONSTRAINT_LT;
  auto a_cost =
      DbSqliteModule::EstimateCost(schema, kRowCount, &info, {0u}, {});

  ASSERT_LT(id_cost.cost, a_cost.cost);
  ASSERT_LT(id_cost.rows, a_cost.rows);
}

TEST(DbSqliteModule, SingleEqCheaperThanMultipleConstraint) {
  auto schema = CreateSchema();
  constexpr uint32_t kRowCount = 1234;

  auto c = CreateConstraint(1, SQLITE_INDEX_CONSTRAINT_EQ);
  auto u = CreateUsage();
  auto info = CreateCsIndexInfo(1, &c, &u);

  auto single_cost =
      DbSqliteModule::EstimateCost(schema, kRowCount, &info, {0u}, {});

  std::array c2{CreateConstraint(1, SQLITE_INDEX_CONSTRAINT_EQ),
                CreateConstraint(2, SQLITE_INDEX_CONSTRAINT_EQ)};
  std::array u2{CreateUsage(), CreateUsage()};
  auto info2 = CreateCsIndexInfo(c2.size(), c2.data(), u2.data());

  auto multi_cost =
      DbSqliteModule::EstimateCost(schema, kRowCount, &info2, {0u, 1u}, {});

  // The cost of the single filter should be cheaper (because of our special
  // handling of single equality). But the number of rows should be greater.
  ASSERT_LT(single_cost.cost, multi_cost.cost);
  ASSERT_GT(single_cost.rows, multi_cost.rows);
}

TEST(DbSqliteModule, MultiSortedEqCheaperThanMultiUnsortedEq) {
  auto schema = CreateSchema();
  constexpr uint32_t kRowCount = 1234;

  std::array c1{CreateConstraint(1, SQLITE_INDEX_CONSTRAINT_EQ),
                CreateConstraint(2, SQLITE_INDEX_CONSTRAINT_EQ)};
  std::array u1{CreateUsage(), CreateUsage()};
  auto info1 = CreateCsIndexInfo(c1.size(), c1.data(), u1.data());

  auto sorted_cost =
      DbSqliteModule::EstimateCost(schema, kRowCount, &info1, {0u, 1u}, {});

  std::array c2{CreateConstraint(3, SQLITE_INDEX_CONSTRAINT_EQ),
                CreateConstraint(4, SQLITE_INDEX_CONSTRAINT_EQ)};
  std::array u2{CreateUsage(), CreateUsage()};
  auto info2 = CreateCsIndexInfo(c2.size(), c2.data(), u2.data());

  auto unsorted_cost =
      DbSqliteModule::EstimateCost(schema, kRowCount, &info2, {0u, 1u}, {});

  // The number of rows should be the same but the cost of the sorted
  // query should be less.
  ASSERT_LT(sorted_cost.cost, unsorted_cost.cost);
  ASSERT_EQ(sorted_cost.rows, unsorted_cost.rows);
}

TEST(DbSqliteModule, EmptyTableCosting) {
  auto schema = CreateSchema();
  constexpr uint32_t kRowCount = 0;

  std::array c1{CreateConstraint(0, SQLITE_INDEX_CONSTRAINT_EQ)};
  std::array u1{CreateUsage()};
  auto info1 = CreateCsIndexInfo(c1.size(), c1.data(), u1.data());

  auto id_cost =
      DbSqliteModule::EstimateCost(schema, kRowCount, &info1, {0u}, {});

  std::array c2{CreateConstraint(0, SQLITE_INDEX_CONSTRAINT_EQ)};
  std::array u2{CreateUsage()};
  auto info2 = CreateCsIndexInfo(c2.size(), c2.data(), u2.data());

  auto a_cost =
      DbSqliteModule::EstimateCost(schema, kRowCount, &info2, {0u}, {});

  ASSERT_DOUBLE_EQ(id_cost.cost, a_cost.cost);
  ASSERT_EQ(id_cost.rows, a_cost.rows);
}

TEST(DbSqliteModule, OrderByOnSortedCheaper) {
  auto schema = CreateSchema();
  constexpr uint32_t kRowCount = 1234;

  sqlite3_index_info::sqlite3_index_orderby ob1{1u, false};
  auto info1 = CreateObIndexInfo(1, &ob1);

  auto a_cost =
      DbSqliteModule::EstimateCost(schema, kRowCount, &info1, {}, {0u});

  sqlite3_index_info::sqlite3_index_orderby ob2{2u, false};
  auto info2 = CreateObIndexInfo(1, &ob2);

  // On an ordered column, the constraint for sorting would get pruned so
  // we would end up with an empty constraint set.
  auto sorted_cost =
      DbSqliteModule::EstimateCost(schema, kRowCount, &info2, {}, {});

  ASSERT_LT(sorted_cost.cost, a_cost.cost);
  ASSERT_EQ(sorted_cost.rows, a_cost.rows);
}

}  // namespace
}  // namespace perfetto::trace_processor
