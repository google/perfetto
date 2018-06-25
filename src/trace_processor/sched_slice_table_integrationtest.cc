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

using Column = SchedSliceTable::Column;

class SchedSliceTableIntegrationTest : public ::testing::Test {
 public:
  SchedSliceTableIntegrationTest() {
    sqlite3_open(":memory:", &db_);

    static sqlite3_module module = SchedSliceTable::CreateModule();
    sqlite3_create_module(db_, "sched", &module, static_cast<void*>(&storage_));
  }

  virtual ~SchedSliceTableIntegrationTest() { sqlite3_close(db_); }

 protected:
  TraceStorage storage_;
  sqlite3* db_;
};

TEST_F(SchedSliceTableIntegrationTest, RowsReturnedInCorrectOrderWithinCpu) {
  uint32_t cpu = 3;
  uint64_t timestamp = 100;
  uint32_t pid_1 = 2;
  uint32_t prev_state = 32;
  static const char kCommProc1[] = "process1";
  static const char kCommProc2[] = "process2";
  uint32_t pid_2 = 4;
  storage_.PushSchedSwitch(cpu, timestamp, pid_1, prev_state, kCommProc1,
                           sizeof(kCommProc1) - 1, pid_2);
  storage_.PushSchedSwitch(cpu, timestamp + 3, pid_2, prev_state, kCommProc2,
                           sizeof(kCommProc2) - 1, pid_1);
  storage_.PushSchedSwitch(cpu, timestamp + 4, pid_1, prev_state, kCommProc1,
                           sizeof(kCommProc1) - 1, pid_2);
  storage_.PushSchedSwitch(cpu, timestamp + 10, pid_2, prev_state, kCommProc2,
                           sizeof(kCommProc2) - 1, pid_1);

  static constexpr char sql[] = "SELECT * from sched ORDER BY dur";

  sqlite3_stmt* stmt;
  ASSERT_EQ(sqlite3_prepare_v2(db_, sql, sizeof(sql) - 1, &stmt, nullptr), 0);

  ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(stmt, Column::kDuration), 1);
  ASSERT_EQ(sqlite3_column_int64(stmt, Column::kTimestamp), timestamp + 3);
  ASSERT_EQ(sqlite3_column_int64(stmt, Column::kCpu), cpu);

  ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(stmt, Column::kDuration), 3);
  ASSERT_EQ(sqlite3_column_int64(stmt, Column::kTimestamp), timestamp);
  ASSERT_EQ(sqlite3_column_int64(stmt, Column::kCpu), cpu);

  ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(stmt, Column::kDuration), 6);
  ASSERT_EQ(sqlite3_column_int64(stmt, Column::kTimestamp), timestamp + 4);
  ASSERT_EQ(sqlite3_column_int64(stmt, Column::kCpu), cpu);

  ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
  sqlite3_finalize(stmt);
}

TEST_F(SchedSliceTableIntegrationTest, RowsReturnedInCorrectOrderBetweenCpu) {
  uint32_t cpu_1 = 3;
  uint32_t cpu_2 = 8;
  uint32_t cpu_3 = 4;
  uint64_t timestamp = 100;
  uint32_t pid_1 = 2;
  uint32_t prev_state = 32;
  static const char kCommProc1[] = "process1";
  static const char kCommProc2[] = "process2";
  uint32_t pid_2 = 4;
  storage_.PushSchedSwitch(cpu_3, timestamp - 2, pid_1, prev_state, kCommProc1,
                           sizeof(kCommProc1) - 1, pid_2);
  storage_.PushSchedSwitch(cpu_3, timestamp - 1, pid_2, prev_state, kCommProc2,
                           sizeof(kCommProc2) - 1, pid_1);
  storage_.PushSchedSwitch(cpu_1, timestamp, pid_1, prev_state, kCommProc1,
                           sizeof(kCommProc1) - 1, pid_2);
  storage_.PushSchedSwitch(cpu_2, timestamp + 3, pid_2, prev_state, kCommProc2,
                           sizeof(kCommProc2) - 1, pid_1);
  storage_.PushSchedSwitch(cpu_1, timestamp + 4, pid_1, prev_state, kCommProc1,
                           sizeof(kCommProc1) - 1, pid_2);
  storage_.PushSchedSwitch(cpu_2, timestamp + 10, pid_2, prev_state, kCommProc2,
                           sizeof(kCommProc2) - 1, pid_1);

  static constexpr char sql[] = "SELECT * from sched ORDER BY dur desc";

  sqlite3_stmt* stmt;
  ASSERT_EQ(sqlite3_prepare_v2(db_, sql, sizeof(sql) - 1, &stmt, nullptr), 0);

  ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(stmt, Column::kDuration), 7);
  ASSERT_EQ(sqlite3_column_int64(stmt, Column::kTimestamp), timestamp + 3);
  ASSERT_EQ(sqlite3_column_int64(stmt, Column::kCpu), cpu_2);

  ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(stmt, Column::kDuration), 4);
  ASSERT_EQ(sqlite3_column_int64(stmt, Column::kTimestamp), timestamp);
  ASSERT_EQ(sqlite3_column_int64(stmt, Column::kCpu), cpu_1);

  ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(stmt, Column::kDuration), 1);
  ASSERT_EQ(sqlite3_column_int64(stmt, Column::kTimestamp), timestamp - 2);
  ASSERT_EQ(sqlite3_column_int64(stmt, Column::kCpu), cpu_3);

  ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
  sqlite3_finalize(stmt);
}

TEST_F(SchedSliceTableIntegrationTest, FilterCpus) {
  uint32_t cpu_1 = 3;
  uint32_t cpu_2 = 8;
  uint64_t timestamp = 100;
  uint32_t pid_1 = 2;
  uint32_t prev_state = 32;
  static const char kCommProc1[] = "process1";
  static const char kCommProc2[] = "process2";
  uint32_t pid_2 = 4;
  storage_.PushSchedSwitch(cpu_1, timestamp, pid_1, prev_state, kCommProc1,
                           sizeof(kCommProc1) - 1, pid_2);
  storage_.PushSchedSwitch(cpu_2, timestamp + 3, pid_2, prev_state, kCommProc2,
                           sizeof(kCommProc2) - 1, pid_1);
  storage_.PushSchedSwitch(cpu_1, timestamp + 4, pid_1, prev_state, kCommProc1,
                           sizeof(kCommProc1) - 1, pid_2);
  storage_.PushSchedSwitch(cpu_2, timestamp + 10, pid_2, prev_state, kCommProc2,
                           sizeof(kCommProc2) - 1, pid_1);

  static constexpr char sql[] = "SELECT * from sched where cpu = 3";

  sqlite3_stmt* stmt;
  ASSERT_EQ(sqlite3_prepare_v2(db_, sql, sizeof(sql) - 1, &stmt, nullptr), 0);

  ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(stmt, Column::kDuration), 4);
  ASSERT_EQ(sqlite3_column_int64(stmt, Column::kTimestamp), timestamp);
  ASSERT_EQ(sqlite3_column_int64(stmt, Column::kCpu), cpu_1);

  ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
  sqlite3_finalize(stmt);
}
}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
