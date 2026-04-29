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

// Verifies the temp-then-promote include pattern: a successful
// `INCLUDE PERFETTO MODULE` issued on the default connection promotes its
// CREATE statements to `main`, so a freshly-minted secondary connection
// (sharing memdb with `cache=shared`) sees the new objects.
TEST(TraceProcessorConnectionTest, IncludePromotesObjectsToOtherConnections) {
  auto tp = TraceProcessor::CreateInstance(Config());
  ASSERT_OK(tp->NotifyEndOfFile());

  SqlPackage pkg;
  pkg.name = "include_promote_test";
  pkg.modules.emplace_back(
      "include_promote_test.tables",
      "CREATE TABLE include_promote_t(id INTEGER, name TEXT);\n"
      "INSERT INTO include_promote_t VALUES (1, 'alpha'), (2, 'beta');\n");
  ASSERT_OK(tp->RegisterSqlPackage(pkg));

  // Issue the include on connection-0 (the default).
  {
    auto it = tp->ExecuteQuery(
        "INCLUDE PERFETTO MODULE include_promote_test.tables;");
    while (it.Next()) {
    }
    ASSERT_OK(it.Status());
  }

  // A secondary connection minted *after* the include should see both rows
  // because the savepoint released the DDL onto `main` and `cache=shared`
  // makes the table visible to all connections backed by the same memdb URI.
  auto conn = tp->CreateConnection();
  ASSERT_NE(conn, nullptr);
  auto it = conn->ExecuteQuery(
      "SELECT id, name FROM include_promote_t ORDER BY id;");
  ASSERT_TRUE(it.Next()) << it.Status().c_message();
  ASSERT_EQ(it.Get(0).long_value, 1);
  ASSERT_STREQ(it.Get(1).string_value, "alpha");
  ASSERT_TRUE(it.Next()) << it.Status().c_message();
  ASSERT_EQ(it.Get(0).long_value, 2);
  ASSERT_STREQ(it.Get(1).string_value, "beta");
  ASSERT_FALSE(it.Next());
  ASSERT_OK(it.Status());
}

// Verifies failed-include atomicity: a module whose body has a bad statement
// at the end must (a) return an error and (b) leave no trace of any CREATE
// statements that ran before the bad one.
TEST(TraceProcessorConnectionTest, FailedIncludeLeavesNoTrace) {
  auto tp = TraceProcessor::CreateInstance(Config());
  ASSERT_OK(tp->NotifyEndOfFile());

  SqlPackage pkg;
  pkg.name = "include_fail_test";
  pkg.modules.emplace_back(
      "include_fail_test.broken",
      "CREATE TABLE include_fail_t(id INTEGER);\n"
      "INSERT INTO include_fail_t VALUES (42);\n"
      // This statement fails — references a column that does not exist.
      "SELECT no_such_column FROM include_fail_t;\n");
  ASSERT_OK(tp->RegisterSqlPackage(pkg));

  // The include should fail.
  {
    auto it = tp->ExecuteQuery(
        "INCLUDE PERFETTO MODULE include_fail_test.broken;");
    while (it.Next()) {
    }
    ASSERT_FALSE(it.Status().ok());
  }

  // After the failed include, the table created earlier in the body must
  // not exist on `main`. Query sqlite_master to verify.
  {
    auto it = tp->ExecuteQuery(
        "SELECT count(*) FROM sqlite_master WHERE name = 'include_fail_t';");
    ASSERT_TRUE(it.Next()) << it.Status().c_message();
    ASSERT_EQ(it.Get(0).long_value, 0);
  }

  // A secondary connection minted now must also not see the table.
  auto conn = tp->CreateConnection();
  ASSERT_NE(conn, nullptr);
  auto it = conn->ExecuteQuery(
      "SELECT count(*) FROM sqlite_master WHERE name = 'include_fail_t';");
  ASSERT_TRUE(it.Next()) << it.Status().c_message();
  ASSERT_EQ(it.Get(0).long_value, 0);
}

// Verifies sequential successful includes accumulate correctly: two distinct
// successful includes from connection-0 should both be visible on a later-
// minted secondary connection.
TEST(TraceProcessorConnectionTest, SequentialIncludesPromoteToOtherConnection) {
  auto tp = TraceProcessor::CreateInstance(Config());
  ASSERT_OK(tp->NotifyEndOfFile());

  SqlPackage pkg;
  pkg.name = "include_seq_test";
  pkg.modules.emplace_back(
      "include_seq_test.first",
      "CREATE TABLE include_seq_a(x INTEGER);\n"
      "INSERT INTO include_seq_a VALUES (1);\n");
  pkg.modules.emplace_back(
      "include_seq_test.second",
      "CREATE TABLE include_seq_b(y INTEGER);\n"
      "INSERT INTO include_seq_b VALUES (2);\n");
  ASSERT_OK(tp->RegisterSqlPackage(pkg));

  {
    auto it = tp->ExecuteQuery(
        "INCLUDE PERFETTO MODULE include_seq_test.first;");
    while (it.Next()) {
    }
    ASSERT_OK(it.Status());
  }
  {
    auto it = tp->ExecuteQuery(
        "INCLUDE PERFETTO MODULE include_seq_test.second;");
    while (it.Next()) {
    }
    ASSERT_OK(it.Status());
  }

  auto conn = tp->CreateConnection();
  ASSERT_NE(conn, nullptr);
  auto it = conn->ExecuteQuery(
      "SELECT (SELECT x FROM include_seq_a) + (SELECT y FROM include_seq_b);");
  ASSERT_TRUE(it.Next()) << it.Status().c_message();
  ASSERT_EQ(it.Get(0).long_value, 3);
  ASSERT_FALSE(it.Next());
  ASSERT_OK(it.Status());
}

}  // namespace
}  // namespace perfetto::trace_processor
