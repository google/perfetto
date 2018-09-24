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
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/sched_tracker.h"
#include "src/trace_processor/trace_processor_context.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "src/trace_processor/scoped_db.h"

namespace perfetto {
namespace trace_processor {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;
using Column = SchedSliceTable::Column;

class SchedSliceTableTest : public ::testing::Test {
 public:
  SchedSliceTableTest() {
    sqlite3* db = nullptr;
    PERFETTO_CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
    db_.reset(db);

    context_.storage.reset(new TraceStorage());
    context_.process_tracker.reset(new ProcessTracker(&context_));
    context_.sched_tracker.reset(new SchedTracker(&context_));

    SchedSliceTable::RegisterTable(db_.get(), context_.storage.get());
  }

  void PrepareValidStatement(const std::string& sql) {
    int size = static_cast<int>(sql.size());
    sqlite3_stmt* stmt;
    ASSERT_EQ(sqlite3_prepare_v2(*db_, sql.c_str(), size, &stmt, nullptr),
              SQLITE_OK);
    stmt_.reset(stmt);
  }

  ~SchedSliceTableTest() override { context_.storage->ResetStorage(); }

 protected:
  TraceProcessorContext context_;
  ScopedDb db_;
  ScopedStmt stmt_;
};

TEST_F(SchedSliceTableTest, RowsReturnedInCorrectOrderWithinCpu) {
  uint32_t cpu = 3;
  uint64_t timestamp = 100;
  uint32_t pid_1 = 2;
  uint32_t prev_state = 32;
  static const char kCommProc1[] = "process1";
  static const char kCommProc2[] = "process2";
  uint32_t pid_2 = 4;
  context_.sched_tracker->PushSchedSwitch(cpu, timestamp, pid_1, prev_state,
                                          kCommProc1, pid_2);
  context_.sched_tracker->PushSchedSwitch(cpu, timestamp + 3, pid_2, prev_state,
                                          kCommProc2, pid_1);
  context_.sched_tracker->PushSchedSwitch(cpu, timestamp + 4, pid_1, prev_state,
                                          kCommProc1, pid_2);
  context_.sched_tracker->PushSchedSwitch(cpu, timestamp + 10, pid_2,
                                          prev_state, kCommProc2, pid_1);

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

TEST_F(SchedSliceTableTest, RowsReturnedInCorrectOrderBetweenCpu) {
  uint32_t cpu_1 = 3;
  uint32_t cpu_2 = 8;
  uint32_t cpu_3 = 4;
  uint64_t timestamp = 100;
  uint32_t pid_1 = 2;
  uint32_t prev_state = 32;
  static const char kCommProc1[] = "process1";
  static const char kCommProc2[] = "process2";
  uint32_t pid_2 = 4;
  context_.sched_tracker->PushSchedSwitch(cpu_3, timestamp - 2, pid_1,
                                          prev_state, kCommProc1, pid_2);
  context_.sched_tracker->PushSchedSwitch(cpu_3, timestamp - 1, pid_2,
                                          prev_state, kCommProc2, pid_1);
  context_.sched_tracker->PushSchedSwitch(cpu_1, timestamp, pid_1, prev_state,
                                          kCommProc1, pid_2);
  context_.sched_tracker->PushSchedSwitch(cpu_2, timestamp + 3, pid_2,
                                          prev_state, kCommProc2, pid_1);
  context_.sched_tracker->PushSchedSwitch(cpu_1, timestamp + 4, pid_1,
                                          prev_state, kCommProc1, pid_2);
  context_.sched_tracker->PushSchedSwitch(cpu_2, timestamp + 10, pid_2,
                                          prev_state, kCommProc2, pid_1);

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

TEST_F(SchedSliceTableTest, FilterCpus) {
  uint32_t cpu_1 = 3;
  uint32_t cpu_2 = 8;
  uint64_t timestamp = 100;
  uint32_t pid_1 = 2;
  uint32_t prev_state = 32;
  static const char kCommProc1[] = "process1";
  static const char kCommProc2[] = "process2";
  uint32_t pid_2 = 4;
  context_.sched_tracker->PushSchedSwitch(cpu_1, timestamp, pid_1, prev_state,
                                          kCommProc1, pid_2);
  context_.sched_tracker->PushSchedSwitch(cpu_2, timestamp + 3, pid_2,
                                          prev_state, kCommProc2, pid_1);
  context_.sched_tracker->PushSchedSwitch(cpu_1, timestamp + 4, pid_1,
                                          prev_state, kCommProc1, pid_2);
  context_.sched_tracker->PushSchedSwitch(cpu_2, timestamp + 10, pid_2,
                                          prev_state, kCommProc2, pid_1);

  PrepareValidStatement("SELECT dur, ts, cpu FROM sched WHERE cpu = 3");

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 0), 4 /* duration */);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 1), timestamp);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 2), cpu_1);

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_DONE);
}

TEST_F(SchedSliceTableTest, UtidTest) {
  uint32_t cpu = 3;
  uint64_t timestamp = 100;
  uint32_t pid_1 = 2;
  uint32_t prev_state = 32;
  static const char kCommProc1[] = "process1";
  static const char kCommProc2[] = "process2";
  uint32_t pid_2 = 4;
  context_.sched_tracker->PushSchedSwitch(cpu, timestamp, pid_1, prev_state,
                                          kCommProc1, pid_2);
  context_.sched_tracker->PushSchedSwitch(cpu, timestamp + 3, pid_2, prev_state,
                                          kCommProc2, pid_1);
  context_.sched_tracker->PushSchedSwitch(cpu, timestamp + 4, pid_1, prev_state,
                                          kCommProc1, pid_2);
  context_.sched_tracker->PushSchedSwitch(cpu, timestamp + 10, pid_2,
                                          prev_state, kCommProc2, pid_1);

  PrepareValidStatement("SELECT utid FROM sched ORDER BY utid");

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 0), 1 /* duration */);

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 0), 1 /* duration */);

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(*stmt_, 0), 2 /* duration */);

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_DONE);
}

TEST_F(SchedSliceTableTest, TimestampFiltering) {
  uint32_t cpu_5 = 5;
  uint32_t cpu_7 = 7;
  uint32_t pid_1 = 1;
  uint32_t pid_2 = 2;
  uint32_t prev_state = 32;

  // Fill |cpu_5| and |cpu_7) with one sched switch per time unit starting,
  // respectively, @ T=50 and T=70.
  for (uint64_t i = 0; i <= 11; i++) {
    context_.sched_tracker->PushSchedSwitch(cpu_5, 50 + i, pid_1, prev_state,
                                            "pid_1", pid_1);
  }
  for (uint64_t i = 0; i <= 11; i++) {
    context_.sched_tracker->PushSchedSwitch(cpu_7, 70 + i, pid_2, prev_state,
                                            "pid_2", pid_2);
  }

  auto query = [this](const std::string& where_clauses) {
    PrepareValidStatement("SELECT ts from sched WHERE " + where_clauses);
    std::vector<int> res;
    while (sqlite3_step(*stmt_) == SQLITE_ROW) {
      res.push_back(sqlite3_column_int(*stmt_, 0));
    }
    return res;
  };

  ASSERT_THAT(query("ts > 55 and ts <= 60"), ElementsAre(56, 57, 58, 59, 60));
  ASSERT_THAT(query("ts >= 55 and ts < 52"), IsEmpty());
  ASSERT_THAT(query("ts >= 70 and ts < 71"), ElementsAre(70));
  ASSERT_THAT(query("ts >= 59 and ts < 73"), ElementsAre(59, 60, 70, 71, 72));
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
