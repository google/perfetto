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

#include "src/trace_processor/thread_table.h"
#include "src/trace_processor/process_table.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/proto_trace_parser.h"
#include "src/trace_processor/sched_tracker.h"
#include "src/trace_processor/scoped_db.h"
#include "src/trace_processor/trace_processor_context.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace perfetto {
namespace trace_processor {
namespace {

class ThreadTableUnittest : public ::testing::Test {
 public:
  ThreadTableUnittest() {
    sqlite3* db = nullptr;
    PERFETTO_CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
    db_.reset(db);

    context_.storage.reset(new TraceStorage());
    context_.process_tracker.reset(new ProcessTracker(&context_));
    context_.sched_tracker.reset(new SchedTracker(&context_));

    ThreadTable::RegisterTable(db_.get(), context_.storage.get());
    ProcessTable::RegisterTable(db_.get(), context_.storage.get());
  }

  void PrepareValidStatement(const std::string& sql) {
    int size = static_cast<int>(sql.size());
    sqlite3_stmt* stmt;
    ASSERT_EQ(sqlite3_prepare_v2(*db_, sql.c_str(), size, &stmt, nullptr),
              SQLITE_OK);
    stmt_.reset(stmt);
  }

  const char* GetColumnAsText(int colId) {
    return reinterpret_cast<const char*>(sqlite3_column_text(*stmt_, colId));
  }

  ~ThreadTableUnittest() override { context_.storage->ResetStorage(); }

 protected:
  TraceProcessorContext context_;
  ScopedDb db_;
  ScopedStmt stmt_;
};

TEST_F(ThreadTableUnittest, Select) {
  uint32_t cpu = 3;
  uint64_t timestamp = 100;
  uint32_t pid_1 = 1;
  uint32_t prev_state = 32;
  static const char kCommProc1[] = "thread1";
  static const char kCommProc2[] = "thread2";
  uint32_t pid_2 = 4;

  context_.sched_tracker->PushSchedSwitch(cpu, timestamp, pid_1, prev_state,
                                          kCommProc1, sizeof(kCommProc1) - 1,
                                          pid_2);
  context_.sched_tracker->PushSchedSwitch(cpu, timestamp + 1, pid_2, prev_state,
                                          kCommProc2, sizeof(kCommProc2) - 1,
                                          pid_1);

  context_.process_tracker->UpdateProcess(2, "test", 4);
  context_.process_tracker->UpdateThread(1, 2);
  PrepareValidStatement("SELECT utid, upid, name FROM thread");

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int(*stmt_, 0), 1 /* utid */);
  ASSERT_EQ(sqlite3_column_int(*stmt_, 1), 1 /* upid */);
  ASSERT_STREQ(GetColumnAsText(2), kCommProc1);

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_DONE);
}

TEST_F(ThreadTableUnittest, SelectWhere) {
  uint32_t cpu = 3;
  uint64_t timestamp = 100;
  uint32_t pid_1 = 1;
  uint32_t prev_state = 32;
  static const char kCommProc1[] = "thread1";
  static const char kCommProc2[] = "thread2";
  uint32_t pid_2 = 4;

  context_.sched_tracker->PushSchedSwitch(cpu, timestamp, pid_1, prev_state,
                                          kCommProc1, sizeof(kCommProc1) - 1,
                                          pid_2);
  context_.sched_tracker->PushSchedSwitch(cpu, timestamp + 1, pid_2, prev_state,
                                          kCommProc2, sizeof(kCommProc2) - 1,
                                          pid_1);
  context_.sched_tracker->PushSchedSwitch(cpu, timestamp + 2, pid_1, prev_state,
                                          kCommProc1, sizeof(kCommProc1) - 1,
                                          pid_2);

  context_.process_tracker->UpdateProcess(2, "test", 4);
  context_.process_tracker->UpdateThread(1, 2);
  context_.process_tracker->UpdateThread(2, 2);
  PrepareValidStatement("SELECT utid, upid, name FROM thread where utid = 1");

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int(*stmt_, 0), 1 /* utid */);
  ASSERT_EQ(sqlite3_column_int(*stmt_, 1), 1 /* upid */);
  ASSERT_STREQ(GetColumnAsText(2), kCommProc1);

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_DONE);
}

TEST_F(ThreadTableUnittest, JoinWithProcess) {
  uint32_t cpu = 3;
  uint64_t timestamp = 100;
  uint32_t pid_1 = 1;
  uint32_t prev_state = 32;
  static const char kCommProc1[] = "thread1";
  static const char kCommProc2[] = "thread2";
  uint32_t pid_2 = 4;

  context_.sched_tracker->PushSchedSwitch(cpu, timestamp, pid_1, prev_state,
                                          kCommProc1, sizeof(kCommProc1) - 1,
                                          pid_2);
  context_.sched_tracker->PushSchedSwitch(cpu, timestamp + 1, pid_2, prev_state,
                                          kCommProc2, sizeof(kCommProc2) - 1,
                                          pid_1);

  context_.process_tracker->UpdateProcess(2, "test", 4);
  context_.process_tracker->UpdateProcess(3, "test1", 5);
  context_.process_tracker->UpdateThread(1, 2);
  PrepareValidStatement(
      "SELECT utid, thread.name, process.upid, process.name FROM thread INNER "
      "JOIN process USING (upid)");

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int(*stmt_, 0), 1 /* utid */);
  ASSERT_STREQ(GetColumnAsText(1), kCommProc1);

  ASSERT_EQ(sqlite3_column_int(*stmt_, 2), 1 /* upid */);
  ASSERT_STREQ(GetColumnAsText(3), "test");

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_DONE);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
