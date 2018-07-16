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
#include "src/trace_processor/scoped_db.h"

namespace perfetto {
namespace trace_processor {
namespace {

using Column = SchedSliceTable::Column;

class SchedSliceTableIntegrationTest : public ::testing::Test {
 public:
  SchedSliceTableIntegrationTest() {
    sqlite3* db = nullptr;
    PERFETTO_CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
    db_.reset(db);

    static sqlite3_module module = SchedSliceTable::CreateModule();
    sqlite3_create_module(*db_, "sched", &module,
                          static_cast<void*>(&storage_));
  }

  void PrepareValidStatement(const std::string& sql) {
    int size = static_cast<int>(sql.size());
    sqlite3_stmt* stmt;
    ASSERT_EQ(sqlite3_prepare_v2(*db_, sql.c_str(), size, &stmt, nullptr),
              SQLITE_OK);
    stmt_.reset(stmt);
  }

 protected:
  TraceStorage storage_;
  ScopedDb db_;
  ScopedStmt stmt_;
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

  PrepareValidStatement("SELECT dur, ts, cpu FROM sched ORDER BY dur");

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 0), 1 /* duration */);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 1), timestamp + 3);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 2), cpu);

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 0), 3 /* duration */);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 1), timestamp);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 2), cpu);

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 0), 6 /* duration */);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 1), timestamp + 4);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 2), cpu);

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_DONE);
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

  PrepareValidStatement("SELECT dur, ts, cpu FROM sched ORDER BY dur desc");

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 0), 7 /* duration */);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 1), timestamp + 3);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 2), cpu_2);

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 0), 4 /* duration */);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 1), timestamp);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 2), cpu_1);

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 0), 1 /* duration */);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 1), timestamp - 2);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 2), cpu_3);

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_DONE);
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

  PrepareValidStatement("SELECT dur, ts, cpu FROM sched WHERE cpu = 3");

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 0), 4 /* duration */);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 1), timestamp);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 2), cpu_1);

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_DONE);
}

TEST_F(SchedSliceTableIntegrationTest, QuanitsiationCpuNativeOrder) {
  uint32_t cpu_1 = 3;
  uint32_t cpu_2 = 8;
  uint64_t timestamp = 100;
  uint32_t pid_1 = 2;
  uint32_t prev_state = 32;
  static const char kCommProc1[] = "process1";
  static const char kCommProc2[] = "process2";
  uint32_t pid_2 = 4;
  storage_.PushSchedSwitch(cpu_2, timestamp, pid_1, prev_state, kCommProc1,
                           sizeof(kCommProc1) - 1, pid_2);
  storage_.PushSchedSwitch(cpu_1, timestamp + 3, pid_2, prev_state, kCommProc2,
                           sizeof(kCommProc2) - 1, pid_1);
  storage_.PushSchedSwitch(cpu_2, timestamp + 4, pid_1, prev_state, kCommProc1,
                           sizeof(kCommProc1) - 1, pid_2);
  storage_.PushSchedSwitch(cpu_1, timestamp + 10, pid_2, prev_state, kCommProc2,
                           sizeof(kCommProc2) - 1, pid_1);

  PrepareValidStatement(
      "SELECT dur, ts, cpu FROM sched WHERE _quantum MATCH 5 ORDER BY cpu");

  // Event at ts + 3 sliced off at quantum boundary (105).
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 0), 2 /* duration */);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 1), timestamp + 3);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 2), cpu_1);

  // Remainder of event at ts + 3 after quantum boundary (105 onwards).
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 0), 5 /* duration */);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 1), timestamp + 5);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 2), cpu_1);

  // Full event at ts.
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 0), 4 /* duration */);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 1), timestamp);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 2), cpu_2);

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_DONE);
}

TEST_F(SchedSliceTableIntegrationTest, QuantizationSqliteDurationOrder) {
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

  PrepareValidStatement(
      "SELECT dur, ts, cpu FROM sched WHERE _quantum match 5 ORDER BY dur");

  // Event at ts + 3 sliced off at quantum boundary (105).
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 0), 2 /* duration */);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 1), timestamp + 3);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 2), cpu_2);

  // Full event at ts.
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 0), 4 /* duration */);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 1), timestamp);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 2), cpu_1);

  // Remainder of event at ts + 3 after quantum boundary (105 onwards).
  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 0), 5 /* duration */);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 1), timestamp + 5);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 2), cpu_2);

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_DONE);
}

TEST_F(SchedSliceTableIntegrationTest, QuantizationGroupAndSum) {
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

  PrepareValidStatement(
      "SELECT SUM(dur) as sum_dur "
      "FROM sched "
      "WHERE _quantum match 5 "
      "GROUP BY quantized_group "
      "ORDER BY sum_dur");

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 0), 5 /* SUM(duration) */);

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 0), 6 /* SUM(duration) */);

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_DONE);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
