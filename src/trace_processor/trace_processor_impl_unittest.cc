/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/iterator.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

using ::testing::HasSubstr;

// Each row takes a few SQLite virtual machine steps, so the rows left after
// the first one take far more steps than the interval between interrupt
// checks.
constexpr int64_t kLongQueryRows = 100000;
constexpr char kLongQuery[] =
    "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c "
    "WHERE x < 100000) SELECT x FROM c";

std::unique_ptr<TraceProcessor> CreateTraceProcessor() {
  auto tp = TraceProcessor::CreateInstance(Config());
  PERFETTO_CHECK(tp->NotifyEndOfFile().ok());
  return tp;
}

// Steps |it| to the end and returns the number of rows it produced.
int64_t CountRows(Iterator& it) {
  int64_t rows = 0;
  while (it.Next()) {
    ++rows;
  }
  return rows;
}

// Reads the first row of |it|, interrupts the query and steps to the end.
void InterruptAfterFirstRow(TraceProcessor* tp, Iterator& it) {
  ASSERT_TRUE(it.Next());
  tp->InterruptQuery();
  CountRows(it);
}

TEST(TraceProcessorImplTest, InterruptStopsRunningQuery) {
  auto tp = CreateTraceProcessor();
  auto it = tp->ExecuteQuery(kLongQuery);
  InterruptAfterFirstRow(tp.get(), it);
  EXPECT_THAT(it.Status().message(), HasSubstr("interrupted"));
}

TEST(TraceProcessorImplTest, QueryAfterInterruptRunsNormally) {
  auto tp = CreateTraceProcessor();
  {
    auto it = tp->ExecuteQuery(kLongQuery);
    InterruptAfterFirstRow(tp.get(), it);
    ASSERT_FALSE(it.Status().ok());
  }
  auto it = tp->ExecuteQuery(kLongQuery);
  EXPECT_EQ(CountRows(it), kLongQueryRows);
  EXPECT_TRUE(it.Status().ok()) << it.Status().message();
}

// As before, an interrupt arriving while nothing runs is forgotten when the
// next query starts.
TEST(TraceProcessorImplTest, InterruptWhileIdleDoesNotAffectNextQuery) {
  auto tp = CreateTraceProcessor();
  tp->InterruptQuery();
  auto it = tp->ExecuteQuery(kLongQuery);
  EXPECT_EQ(CountRows(it), kLongQueryRows);
  EXPECT_TRUE(it.Status().ok()) << it.Status().message();
}

// Computing metrics forgets an earlier interrupt too, as a query does.
TEST(TraceProcessorImplTest, InterruptWhileIdleDoesNotAffectNextMetric) {
  auto tp = CreateTraceProcessor();
  tp->InterruptQuery();
  std::vector<uint8_t> metrics;
  base::Status status = tp->ComputeMetric({"trace_metadata"}, &metrics);
  EXPECT_TRUE(status.ok()) << status.message();
}

// An interrupt arriving between two statements of a query, while no statement
// runs, stops the next statement before it starts, however short it is.
TEST(TraceProcessorImplTest, InterruptBetweenStatementsStopsNextStatement) {
  auto tp = CreateTraceProcessor();
  std::string sql = "SELECT 1; SELECT 2";
  uint32_t offset = 0;

  std::optional<Iterator> first = tp->ExecuteNextStatement(sql, &offset);
  ASSERT_TRUE(first.has_value());
  CountRows(*first);
  ASSERT_TRUE(first->Status().ok()) << first->Status().message();
  // As in the RPC server, the first statement's iterator is gone before the
  // next statement starts.
  first.reset();

  tp->InterruptQuery();

  std::optional<Iterator> second = tp->ExecuteNextStatement(sql, &offset);
  ASSERT_TRUE(second.has_value());
  CountRows(*second);
  EXPECT_THAT(second->Status().message(), HasSubstr("interrupted"));
}

// RestoreInitialTables() replaces the SQLite connection, which must keep
// checking for interrupts.
TEST(TraceProcessorImplTest, InterruptStopsQueryAfterRestoreInitialTables) {
  auto tp = CreateTraceProcessor();
  tp->RestoreInitialTables();
  auto it = tp->ExecuteQuery(kLongQuery);
  InterruptAfterFirstRow(tp.get(), it);
  EXPECT_THAT(it.Status().message(), HasSubstr("interrupted"));
}

}  // namespace
}  // namespace perfetto::trace_processor
