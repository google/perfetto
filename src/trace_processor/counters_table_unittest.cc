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

#include "src/trace_processor/counters_table.h"
#include "src/trace_processor/sched_tracker.h"
#include "src/trace_processor/scoped_db.h"
#include "src/trace_processor/trace_processor_context.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace perfetto {
namespace trace_processor {
namespace {

class CountersTableUnittest : public ::testing::Test {
 public:
  CountersTableUnittest() {
    sqlite3* db = nullptr;
    PERFETTO_CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
    db_.reset(db);

    context_.storage.reset(new TraceStorage());
    context_.sched_tracker.reset(new SchedTracker(&context_));

    CountersTable::RegisterTable(db_.get(), context_.storage.get());
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

  ~CountersTableUnittest() override { context_.storage->ResetStorage(); }

 protected:
  TraceProcessorContext context_;
  ScopedDb db_;
  ScopedStmt stmt_;
};

TEST_F(CountersTableUnittest, SelectWhereCpu) {
  uint64_t timestamp = 1000;
  uint32_t freq = 3000;
  context_.storage->mutable_counters()->AddCounter(
      timestamp, 0, 1, freq, 0, 1 /* cpu */, RefType::kCPU_ID);
  context_.storage->mutable_counters()->AddCounter(
      timestamp + 1, 1, 1, freq + 1000, 1000, 1 /* cpu */, RefType::kCPU_ID);
  context_.storage->mutable_counters()->AddCounter(
      timestamp + 2, 1, 1, freq + 2000, 1000, 2 /* cpu */, RefType::kCPU_ID);

  PrepareValidStatement("SELECT ts, dur, value FROM counters where ref = 1");

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int(*stmt_, 0), timestamp);
  ASSERT_EQ(sqlite3_column_int(*stmt_, 1), 0);
  ASSERT_EQ(sqlite3_column_int(*stmt_, 2), freq);

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int(*stmt_, 0), timestamp + 1);
  ASSERT_EQ(sqlite3_column_int(*stmt_, 1), 1);
  ASSERT_EQ(sqlite3_column_int(*stmt_, 2), freq + 1000);

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_DONE);
}

TEST_F(CountersTableUnittest, GroupByFreq) {
  uint64_t timestamp = 1000;
  uint32_t freq = 3000;
  uint32_t name_id = 1;
  context_.storage->mutable_counters()->AddCounter(
      timestamp, 1 /* dur */, name_id, freq, 0 /* value delta */, 1 /* cpu */,
      RefType::kCPU_ID);
  context_.storage->mutable_counters()->AddCounter(
      timestamp + 1, 2 /* dur */, name_id, freq + 1000, 1000 /* value delta */,
      1 /* cpu */, RefType::kCPU_ID);
  context_.storage->mutable_counters()->AddCounter(
      timestamp + 3, 0 /* dur */, name_id, freq, -1000 /* value delta */,
      1 /* cpu */, RefType::kCPU_ID);

  PrepareValidStatement(
      "SELECT value, sum(dur) as dur_sum FROM counters where value > 0 group "
      "by value order by dur_sum desc");

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int(*stmt_, 0), freq + 1000);
  ASSERT_EQ(sqlite3_column_int(*stmt_, 1), 2);

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int(*stmt_, 0), freq);
  ASSERT_EQ(sqlite3_column_int(*stmt_, 1), 1);

  ASSERT_EQ(sqlite3_step(*stmt_), SQLITE_DONE);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
