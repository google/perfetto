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

#include "src/trace_processor/perfetto_sql/intrinsics/operators/span_join_operator.h"

#include <sqlite3.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/sqlite/scoped_db.h"
#include "src/trace_processor/sqlite/sqlite_engine.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

class SpanJoinOperatorTableTest : public ::testing::Test {
 public:
  SpanJoinOperatorTableTest() {
    engine_.sqlite_engine()->RegisterVirtualTableModule<SpanJoinOperatorModule>(
        "span_join",
        std::make_unique<SpanJoinOperatorModule::Context>(&engine_));
    engine_.sqlite_engine()->RegisterVirtualTableModule<SpanJoinOperatorModule>(
        "span_left_join",
        std::make_unique<SpanJoinOperatorModule::Context>(&engine_));
  }

  void PrepareValidStatement(const std::string& sql) {
    int size = static_cast<int>(sql.size());
    sqlite3_stmt* stmt;
    ASSERT_EQ(sqlite3_prepare_v2(engine_.sqlite_engine()->db(), sql.c_str(),
                                 size, &stmt, nullptr),
              SQLITE_OK);
    stmt_.reset(stmt);
  }

  void RunStatement(const std::string& sql) {
    PrepareValidStatement(sql);
    ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_DONE);
  }

  void AssertNextRow(const std::vector<int64_t>& elements) {
    ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_ROW);
    for (size_t i = 0; i < elements.size(); ++i) {
      ASSERT_EQ(sqlite3_column_int64(stmt_.get(), static_cast<int>(i)),
                elements[i]);
    }
  }

 protected:
  StringPool pool_;
  PerfettoSqlEngine engine_{&pool_, true};
  ScopedStmt stmt_;
};

TEST_F(SpanJoinOperatorTableTest, JoinTwoSpanTables) {
  RunStatement(
      "CREATE TEMP TABLE f("
      "ts BIGINT PRIMARY KEY, "
      "dur BIGINT, "
      "cpu UNSIGNED INT"
      ");");
  RunStatement(
      "CREATE TEMP TABLE s("
      "ts BIGINT PRIMARY KEY, "
      "dur BIGINT, "
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
      "ts BIGINT PRIMARY KEY, "
      "dur BIGINT, "
      "cpu UNSIGNED INT"
      ");");
  RunStatement(
      "CREATE TEMP TABLE s("
      "ts BIGINT PRIMARY KEY, "
      "dur BIGINT, "
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

TEST_F(SpanJoinOperatorTableTest, MixedPartitioning) {
  RunStatement(
      "CREATE TEMP TABLE f("
      "ts BIGINT PRIMARY KEY, "
      "dur BIGINT, "
      "upid UNSIGNED INT"
      ");");
  RunStatement(
      "CREATE TEMP TABLE s("
      "ts BIGINT PRIMARY KEY, "
      "dur BIGINT, "
      "s_val BIGINT"
      ");");
  RunStatement(
      "CREATE VIRTUAL TABLE sp USING span_join(f PARTITIONED upid, s);");

  RunStatement("INSERT INTO f VALUES(30, 20, NULL);");
  RunStatement("INSERT INTO f VALUES(100, 10, 5);");
  RunStatement("INSERT INTO f VALUES(110, 50, 5);");
  RunStatement("INSERT INTO f VALUES(120, 100, 2);");
  RunStatement("INSERT INTO f VALUES(160, 10, 5);");
  RunStatement("INSERT INTO f VALUES(300, 100, 2);");

  RunStatement("INSERT INTO s VALUES(100, 5, 11111);");
  RunStatement("INSERT INTO s VALUES(105, 5, 22222);");
  RunStatement("INSERT INTO s VALUES(110, 60, 33333);");
  RunStatement("INSERT INTO s VALUES(320, 10, 44444);");

  PrepareValidStatement("SELECT * FROM sp");
  AssertNextRow({120, 50, 2, 33333});
  AssertNextRow({320, 10, 2, 44444});
  AssertNextRow({100, 5, 5, 11111});
  AssertNextRow({105, 5, 5, 22222});
  AssertNextRow({110, 50, 5, 33333});
  AssertNextRow({160, 10, 5, 33333});
  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_DONE);
}

TEST_F(SpanJoinOperatorTableTest, NoPartitioning) {
  RunStatement(
      "CREATE TEMP TABLE f("
      "ts BIGINT PRIMARY KEY, "
      "dur BIGINT, "
      "f_val BIGINT"
      ");");
  RunStatement(
      "CREATE TEMP TABLE s("
      "ts BIGINT PRIMARY KEY, "
      "dur BIGINT, "
      "s_val BIGINT"
      ");");
  RunStatement("CREATE VIRTUAL TABLE sp USING span_join(f, s);");

  RunStatement("INSERT INTO f VALUES(100, 10, 44444);");
  RunStatement("INSERT INTO f VALUES(110, 50, 55555);");
  RunStatement("INSERT INTO f VALUES(160, 10, 44444);");

  RunStatement("INSERT INTO s VALUES(100, 5, 11111);");
  RunStatement("INSERT INTO s VALUES(105, 5, 22222);");
  RunStatement("INSERT INTO s VALUES(110, 60, 33333);");

  PrepareValidStatement("SELECT * FROM sp");
  AssertNextRow({100, 5, 44444, 11111});
  AssertNextRow({105, 5, 44444, 22222});
  AssertNextRow({110, 50, 55555, 33333});
  AssertNextRow({160, 10, 44444, 33333});
  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_DONE);
}

TEST_F(SpanJoinOperatorTableTest, LeftJoinTwoSpanTables) {
  RunStatement(
      "CREATE TEMP TABLE f("
      "ts BIGINT PRIMARY KEY, "
      "dur BIGINT, "
      "cpu UNSIGNED INT"
      ");");
  RunStatement(
      "CREATE TEMP TABLE s("
      "ts BIGINT PRIMARY KEY, "
      "dur BIGINT, "
      "tid UNSIGNED INT"
      ");");
  RunStatement("CREATE VIRTUAL TABLE sp USING span_left_join(f, s);");

  RunStatement("INSERT INTO f VALUES(100, 10, 0);");
  RunStatement("INSERT INTO f VALUES(110, 50, 1);");

  RunStatement("INSERT INTO s VALUES(100, 5, 1);");
  RunStatement("INSERT INTO s VALUES(110, 40, 2);");
  RunStatement("INSERT INTO s VALUES(150, 50, 3);");

  PrepareValidStatement("SELECT * FROM sp");

  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 0), 100);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 1), 5);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 2), 0);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 3), 1);

  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 0), 105);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 1), 5);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 2), 0);
  ASSERT_EQ(sqlite3_column_type(stmt_.get(), 3), SQLITE_NULL);

  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 0), 110);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 1), 40);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 2), 1);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 3), 2);

  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 0), 150);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 1), 10);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 2), 1);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 3), 3);

  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_DONE);
}

TEST_F(SpanJoinOperatorTableTest, LeftJoinTwoSpanTables_EmptyRight) {
  RunStatement(
      "CREATE TEMP TABLE f("
      "ts BIGINT PRIMARY KEY, "
      "dur BIGINT, "
      "cpu UNSIGNED INT"
      ");");
  RunStatement(
      "CREATE TEMP TABLE s("
      "ts BIGINT PRIMARY KEY, "
      "dur BIGINT, "
      "tid UNSIGNED INT"
      ");");
  RunStatement("CREATE VIRTUAL TABLE sp USING span_left_join(f, s);");

  RunStatement("INSERT INTO f VALUES(100, 10, 0);");
  RunStatement("INSERT INTO f VALUES(110, 50, 1);");

  PrepareValidStatement("SELECT * FROM sp");

  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 0), 100);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 1), 10);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 2), 0);
  ASSERT_EQ(sqlite3_column_type(stmt_.get(), 3), SQLITE_NULL);

  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 0), 110);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 1), 50);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 2), 1);
  ASSERT_EQ(sqlite3_column_type(stmt_.get(), 3), SQLITE_NULL);

  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_DONE);
}

TEST_F(SpanJoinOperatorTableTest, CapitalizedLeftJoin) {
  RunStatement(
      "CREATE TEMP TABLE f("
      "ts BIGINT PRIMARY KEY, "
      "dur BIGINT, "
      "cpu UNSIGNED INT"
      ");");
  RunStatement(
      "CREATE TEMP TABLE s("
      "ts BIGINT PRIMARY KEY, "
      "dur BIGINT, "
      "tid UNSIGNED INT"
      ");");
  RunStatement("CREATE VIRTUAL TABLE sp USING SPAN_LEFT_JOIN(f, s);");

  RunStatement("INSERT INTO f VALUES(100, 10, 0);");
  RunStatement("INSERT INTO f VALUES(110, 50, 1);");

  PrepareValidStatement("SELECT * FROM sp");

  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 0), 100);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 1), 10);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 2), 0);
  ASSERT_EQ(sqlite3_column_type(stmt_.get(), 3), SQLITE_NULL);

  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_ROW);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 0), 110);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 1), 50);
  ASSERT_EQ(sqlite3_column_int64(stmt_.get(), 2), 1);
  ASSERT_EQ(sqlite3_column_type(stmt_.get(), 3), SQLITE_NULL);

  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_DONE);
}

TEST_F(SpanJoinOperatorTableTest, NoDurationOnOne) {
  RunStatement(
      "CREATE TEMP TABLE f("
      "ts BIGINT PRIMARY KEY, "
      "f_val BIGINT"
      ");");
  RunStatement(
      "CREATE TEMP TABLE s("
      "ts BIGINT PRIMARY KEY, "
      "dur BIGINT, "
      "s_val BIGINT"
      ");");
  RunStatement("CREATE VIRTUAL TABLE sp USING span_join(f, s);");

  RunStatement("INSERT INTO f VALUES(100, 44444);");
  RunStatement("INSERT INTO f VALUES(120, 55555);");
  RunStatement("INSERT INTO f VALUES(140, 66666);");
  RunStatement("INSERT INTO f VALUES(160, 77777);");

  RunStatement("INSERT INTO s VALUES(100, 5, 11111);");
  RunStatement("INSERT INTO s VALUES(110, 20, 22222);");
  RunStatement("INSERT INTO s VALUES(150, 60, 33333);");

  PrepareValidStatement("SELECT * FROM sp");
  AssertNextRow({100, 0, 44444, 11111});
  AssertNextRow({120, 0, 55555, 22222});
  AssertNextRow({160, 0, 77777, 33333});
  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_DONE);
}

TEST_F(SpanJoinOperatorTableTest, ErrorIfNoDurationOnEither) {
  RunStatement(
      "CREATE TEMP TABLE f("
      "ts BIGINT PRIMARY KEY, "
      "f_val BIGINT"
      ");");
  RunStatement(
      "CREATE TEMP TABLE s("
      "ts BIGINT PRIMARY KEY, "
      "s_val BIGINT"
      ");");
  PrepareValidStatement("CREATE VIRTUAL TABLE sp USING span_join(f, s);");
  ASSERT_EQ(sqlite3_step(stmt_.get()), SQLITE_ERROR);
}

}  // namespace
}  // namespace perfetto::trace_processor
