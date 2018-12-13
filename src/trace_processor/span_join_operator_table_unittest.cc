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

#include "src/trace_processor/span_join_operator_table.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {
namespace {

class SpanJoinOperatorTableTest : public ::testing::Test {
 public:
  SpanJoinOperatorTableTest() {
    sqlite3* db = nullptr;
    PERFETTO_CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
    db_.reset(db);

    context_.storage.reset(new TraceStorage());

    SpanJoinOperatorTable::RegisterTable(db_.get(), context_.storage.get());
  }

  void PrepareValidStatement(const std::string& sql) {
    int size = static_cast<int>(sql.size());
    sqlite3_stmt* stmt;
    ASSERT_EQ(sqlite3_prepare_v2(*db_, sql.c_str(), size, &stmt, nullptr),
              SQLITE_OK);
    stmt_.reset(stmt);
  }

  void RunStatement(const std::string& sql) {
    PrepareValidStatement(sql);
    ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_DONE);
  }

  ~SpanJoinOperatorTableTest() override { context_.storage->ResetStorage(); }

 protected:
  TraceProcessorContext context_;
  ScopedDb db_;
  ScopedStmt stmt_;
};

TEST_F(SpanJoinOperatorTableTest, JoinTwoSpanTables) {
  RunStatement(
      "CREATE TEMP TABLE f("
      "ts BIG INT PRIMARY KEY, "
      "dur BIG INT, "
      "cpu UNSIGNED INT"
      ");");
  RunStatement(
      "CREATE TEMP TABLE s("
      "ts BIG INT PRIMARY KEY, "
      "dur BIG INT, "
      "cpu UNSIGNED INT"
      ");");
  RunStatement(
      "CREATE VIRTUAL TABLE sp USING span_join(f PARTITIONED cpu, "
      "s PARTITIONED cpu);");

  RunStatement("INSERT INTO f VALUES(100, 10, 5);");
  RunStatement("INSERT INTO f VALUES(110, 50, 5);");
  RunStatement("INSERT INTO f VALUES(120, 100, 2);");
  RunStatement("INSERT INTO f VALUES(160, 10, 5);");

  RunStatement("INSERT INTO s VALUES(100, 5, 5);");
  RunStatement("INSERT INTO s VALUES(105, 100, 5);");
  RunStatement("INSERT INTO s VALUES(110, 50, 2);");
  RunStatement("INSERT INTO s VALUES(160, 100, 2);");

  PrepareValidStatement("SELECT * FROM sp");

  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 0), 120);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 1), 40);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 2), 2);

  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 0), 160);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 1), 60);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 2), 2);

  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 0), 100);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 1), 5);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 2), 5);

  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 0), 105);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 1), 5);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 2), 5);

  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 0), 110);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 1), 50);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 2), 5);

  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 0), 160);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 1), 10);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 2), 5);

  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_DONE);
}

TEST_F(SpanJoinOperatorTableTest, NullPartitionKey) {
  RunStatement(
      "CREATE TEMP TABLE f("
      "ts BIG INT PRIMARY KEY, "
      "dur BIG INT, "
      "cpu UNSIGNED INT"
      ");");
  RunStatement(
      "CREATE TEMP TABLE s("
      "ts BIG INT PRIMARY KEY, "
      "dur BIG INT, "
      "cpu UNSIGNED INT"
      ");");
  RunStatement(
      "CREATE VIRTUAL TABLE sp USING span_join(f PARTITIONED cpu, "
      "s PARTITIONED cpu);");

  RunStatement("INSERT INTO f VALUES(30, 20, NULL);");
  RunStatement("INSERT INTO f VALUES(100, 10, 5);");
  RunStatement("INSERT INTO f VALUES(110, 50, 5);");
  RunStatement("INSERT INTO f VALUES(120, 100, 2);");
  RunStatement("INSERT INTO f VALUES(160, 10, 5);");

  RunStatement("INSERT INTO s VALUES(40, 10, NULL);");
  RunStatement("INSERT INTO s VALUES(100, 5, 5);");
  RunStatement("INSERT INTO s VALUES(105, 100, 5);");
  RunStatement("INSERT INTO s VALUES(110, 50, 2);");
  RunStatement("INSERT INTO s VALUES(160, 100, 2);");

  PrepareValidStatement("SELECT * FROM sp");

  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 0), 120);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 1), 40);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 2), 2);

  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 0), 160);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 1), 60);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 2), 2);

  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 0), 100);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 1), 5);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 2), 5);

  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 0), 105);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 1), 5);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 2), 5);

  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 0), 110);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 1), 50);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 2), 5);

  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 0), 160);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 1), 10);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 2), 5);

  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_DONE);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
