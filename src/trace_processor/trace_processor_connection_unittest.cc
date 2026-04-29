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

#include <memory>
#include <string>

#include "perfetto/base/status.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/iterator.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/base/test/status_matchers.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

// Smoke tests for `TraceProcessor::CreateConnection` and the secondary
// `Connection` it returns. Phase 2 iter 2 wires the secondary connection
// to its own `PerfettoSqlEngine` opened against the primary engine's
// memdb URI; these tests verify that scaffold end-to-end on a fresh
// (empty) trace.
TEST(TraceProcessorConnectionTest, SecondaryConnectionExecutesTrivialQuery) {
  auto tp = TraceProcessor::CreateInstance(Config());
  ASSERT_OK(tp->NotifyEndOfFile());

  auto conn = tp->CreateConnection();
  ASSERT_NE(conn, nullptr);

  // Trivial query that touches no tables: exercises the per-connection
  // SQLite handle and the Iterator wiring without depending on any
  // schema or vtab/function being replicated to the secondary engine.
  auto it = conn->ExecuteQuery("SELECT 1");
  ASSERT_TRUE(it.Next()) << it.Status().c_message();
  ASSERT_EQ(it.Get(0).type, SqlValue::kLong);
  ASSERT_EQ(it.Get(0).long_value, 1);
  ASSERT_FALSE(it.Next());
  ASSERT_OK(it.Status());
}

TEST(TraceProcessorConnectionTest, SecondaryConnectionSeesPrimarySchema) {
  auto tp = TraceProcessor::CreateInstance(Config());
  ASSERT_OK(tp->NotifyEndOfFile());

  // Create a plain SQL table on connection-0. It lives in `main` and
  // should propagate to other connections via `cache=shared`.
  {
    auto it = tp->ExecuteQuery(
        "CREATE TABLE conn_test_table(id INTEGER, val TEXT);");
    while (it.Next()) {
    }
    ASSERT_OK(it.Status());
  }
  {
    auto it =
        tp->ExecuteQuery("INSERT INTO conn_test_table VALUES(7, 'hello');");
    while (it.Next()) {
    }
    ASSERT_OK(it.Status());
  }

  auto conn = tp->CreateConnection();
  ASSERT_NE(conn, nullptr);

  // The secondary connection should see the table created on conn-0
  // because both handles point at the same shared in-memory database.
  auto it = conn->ExecuteQuery(
      "SELECT id, val FROM conn_test_table ORDER BY id;");
  ASSERT_TRUE(it.Next()) << it.Status().c_message();
  ASSERT_EQ(it.Get(0).type, SqlValue::kLong);
  ASSERT_EQ(it.Get(0).long_value, 7);
  ASSERT_EQ(it.Get(1).type, SqlValue::kString);
  ASSERT_STREQ(it.Get(1).string_value, "hello");
  ASSERT_FALSE(it.Next());
  ASSERT_OK(it.Status());
}

TEST(TraceProcessorConnectionTest, MultipleConnectionsCoexist) {
  auto tp = TraceProcessor::CreateInstance(Config());
  ASSERT_OK(tp->NotifyEndOfFile());

  auto conn_a = tp->CreateConnection();
  auto conn_b = tp->CreateConnection();
  ASSERT_NE(conn_a, nullptr);
  ASSERT_NE(conn_b, nullptr);

  {
    auto it = conn_a->ExecuteQuery("SELECT 1");
    ASSERT_TRUE(it.Next()) << it.Status().c_message();
    ASSERT_EQ(it.Get(0).long_value, 1);
  }
  {
    auto it = conn_b->ExecuteQuery("SELECT 2");
    ASSERT_TRUE(it.Next()) << it.Status().c_message();
    ASSERT_EQ(it.Get(0).long_value, 2);
  }
}

}  // namespace
}  // namespace perfetto::trace_processor
