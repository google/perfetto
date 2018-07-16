/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/trace_processor/sched_slice_table.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace perfetto {
namespace trace_processor {
namespace {

TEST(SchedSliceTableTest, IndexWithNoConstraintsOrderBy) {
  sqlite3_index_info info;
  info.nConstraint = 0;
  info.nOrderBy = 0;

  TraceStorage storage;
  SchedSliceTable table(&storage);
  table.BestIndex(&info);

  ASSERT_EQ(info.orderByConsumed, true);
  sqlite3_free(info.idxStr);
}

TEST(SchedSliceTableTest, IndexWithConstraintsAndOrderBy) {
  sqlite3_index_info::sqlite3_index_constraint constraints[2] = {};
  constraints[0].usable = true;
  constraints[0].op = SQLITE_INDEX_CONSTRAINT_EQ;
  constraints[0].iColumn = SchedSliceTable::Column::kTimestamp;

  constraints[1].usable = false;
  constraints[1].op = SQLITE_INDEX_CONSTRAINT_GE;
  constraints[1].iColumn = SchedSliceTable::Column::kDuration;

  sqlite3_index_info::sqlite3_index_orderby orderby[2] = {};
  orderby[0].iColumn = SchedSliceTable::Column::kTimestamp;
  orderby[0].desc = true;

  orderby[1].iColumn = SchedSliceTable::Column::kCpu;
  orderby[1].desc = false;

  sqlite3_index_info::sqlite3_index_constraint_usage constraint_usage[2] = {};

  sqlite3_index_info info;
  info.nConstraint = sizeof(constraints) / sizeof(constraints[0]);
  info.aConstraint = constraints;
  info.nOrderBy = sizeof(orderby) / sizeof(orderby[0]);
  info.aOrderBy = orderby;
  info.aConstraintUsage = constraint_usage;

  TraceStorage storage;
  SchedSliceTable table(&storage);
  table.BestIndex(&info);

  ASSERT_EQ(info.orderByConsumed, true);
  ASSERT_EQ(info.aConstraintUsage[0].argvIndex, 1);
  ASSERT_EQ(info.aConstraintUsage[1].argvIndex, 0);
  sqlite3_free(info.idxStr);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
