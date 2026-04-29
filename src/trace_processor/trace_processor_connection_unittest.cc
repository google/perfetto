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

// Phase 2 iter 6: every top-level `Execute(sql)` opens a SAVEPOINT
// (`perfetto_execute_<n>`) so that if a later statement in a multi-statement
// SQL string fails, earlier side-effects are rolled back. Verifies the
// rollback path is observed (a) on the issuing connection and (b) on a
// freshly-minted secondary connection (which would otherwise see the
// orphaned table via `cache=shared`).
TEST(TraceProcessorConnectionTest, MultiStatementExecuteRollsBackOnFailure) {
  auto tp = TraceProcessor::CreateInstance(Config());
  ASSERT_OK(tp->NotifyEndOfFile());

  // Two CREATE TABLEs in a single Execute. The second targets the same name
  // as the first and must fail with "table multistmt_t already exists".
  {
    auto it = tp->ExecuteQuery(
        "CREATE TABLE multistmt_t (x INT); "
        "CREATE TABLE multistmt_t (y INT);");
    while (it.Next()) {
    }
    ASSERT_FALSE(it.Status().ok());
  }

  // The first CREATE must have been rolled back by the outer execute
  // savepoint: the table must NOT be present in `sqlite_master` on the
  // primary connection.
  {
    auto it = tp->ExecuteQuery(
        "SELECT count(*) FROM sqlite_master WHERE name = 'multistmt_t';");
    ASSERT_TRUE(it.Next()) << it.Status().c_message();
    ASSERT_EQ(it.Get(0).long_value, 0);
  }

  // A freshly-minted secondary connection must also not see the table.
  // This rules out the case where the rollback only affects the primary
  // engine's view but leaks via `cache=shared` to a secondary.
  auto conn = tp->CreateConnection();
  ASSERT_NE(conn, nullptr);
  auto it = conn->ExecuteQuery(
      "SELECT count(*) FROM sqlite_master WHERE name = 'multistmt_t';");
  ASSERT_TRUE(it.Next()) << it.Status().c_message();
  ASSERT_EQ(it.Get(0).long_value, 0);
}

// Positive control for the execute-savepoint wrap: two successful CREATE
// statements in a single `Execute` land normally and are visible to a
// secondary connection. Together with `MultiStatementExecuteRollsBackOnFailure`
// this asserts the savepoint behaves as RELEASE-on-success / ROLLBACK-on-error.
TEST(TraceProcessorConnectionTest, MultiStatementExecuteCommitsOnSuccess) {
  auto tp = TraceProcessor::CreateInstance(Config());
  ASSERT_OK(tp->NotifyEndOfFile());

  {
    auto it = tp->ExecuteQuery(
        "CREATE TABLE multistmt_ok_a (x INT); "
        "CREATE TABLE multistmt_ok_b (y INT); "
        "INSERT INTO multistmt_ok_a VALUES (7); "
        "INSERT INTO multistmt_ok_b VALUES (11);");
    while (it.Next()) {
    }
    ASSERT_OK(it.Status());
  }

  auto conn = tp->CreateConnection();
  ASSERT_NE(conn, nullptr);
  auto it = conn->ExecuteQuery(
      "SELECT (SELECT x FROM multistmt_ok_a) + "
      "(SELECT y FROM multistmt_ok_b);");
  ASSERT_TRUE(it.Next()) << it.Status().c_message();
  ASSERT_EQ(it.Get(0).long_value, 18);
  ASSERT_FALSE(it.Next());
  ASSERT_OK(it.Status());
}

// Phase 2 iter 4 smoke test: a `CREATE PERFETTO TABLE` on connection-0
// installs a dataframe-backed virtual table. The secondary connection must
// be able to `SELECT` from it and observe the same rows, which exercises:
//  - dataframe vtab module registration on the secondary engine
//  - cross-connection vtab-state publishing via `GlobalStagingArea`
//    (writer publishes its committed `PerVtabState::committed_state`
//    on every `OnCommit`)
//  - cold xConnect resolution on the secondary engine consulting staging
//    via `DataframeModule::Context::ResolveMissingStateOnConnect`
//  - re-resolution at cursor creation time (`DataframeModule::Filter` /
//    `BestIndex` look up the State from staging rather than caching it
//    in `PerVtabState`).
TEST(TraceProcessorConnectionTest,
     SecondaryConnectionReadsDataframeVtabFromPrimary) {
  auto tp = TraceProcessor::CreateInstance(Config());
  ASSERT_OK(tp->NotifyEndOfFile());

  // CREATE PERFETTO TABLE goes through DataframeModule (vs. plain CREATE
  // TABLE which is a regular sqlite btree).
  {
    auto it = tp->ExecuteQuery(
        "CREATE PERFETTO TABLE conn_df_test AS "
        "SELECT 1 AS id, 'first' AS name UNION ALL "
        "SELECT 2 AS id, 'second' AS name UNION ALL "
        "SELECT 3 AS id, 'third' AS name;");
    while (it.Next()) {
    }
    ASSERT_OK(it.Status());
  }

  // Verify the writer connection sees the rows.
  {
    auto it =
        tp->ExecuteQuery("SELECT id, name FROM conn_df_test ORDER BY id;");
    ASSERT_TRUE(it.Next()) << it.Status().c_message();
    ASSERT_EQ(it.Get(0).long_value, 1);
    ASSERT_TRUE(it.Next()) << it.Status().c_message();
    ASSERT_TRUE(it.Next()) << it.Status().c_message();
    ASSERT_FALSE(it.Next());
    ASSERT_OK(it.Status());
  }

  // Now mint a secondary connection and run the same query through it.
  auto conn = tp->CreateConnection();
  ASSERT_NE(conn, nullptr);

  auto it =
      conn->ExecuteQuery("SELECT id, name FROM conn_df_test ORDER BY id;");
  ASSERT_TRUE(it.Next()) << it.Status().c_message();
  ASSERT_EQ(it.Get(0).long_value, 1);
  ASSERT_STREQ(it.Get(1).string_value, "first");
  ASSERT_TRUE(it.Next()) << it.Status().c_message();
  ASSERT_EQ(it.Get(0).long_value, 2);
  ASSERT_STREQ(it.Get(1).string_value, "second");
  ASSERT_TRUE(it.Next()) << it.Status().c_message();
  ASSERT_EQ(it.Get(0).long_value, 3);
  ASSERT_STREQ(it.Get(1).string_value, "third");
  ASSERT_FALSE(it.Next());
  ASSERT_OK(it.Status());
}

// Verifies that secondary connections see static dataframe-backed tables
// (e.g. `thread`, `process`) registered via `RegisterStaticTable` during
// engine init. These are the bread-and-butter tables that any real
// `SELECT * FROM thread` query would hit.
TEST(TraceProcessorConnectionTest,
     SecondaryConnectionReadsStaticDataframeTable) {
  auto tp = TraceProcessor::CreateInstance(Config());
  ASSERT_OK(tp->NotifyEndOfFile());

  // Pick `thread` as the canonical static dataframe-backed table. It has
  // an `_auto_id` PK column plus several user-facing columns; an empty
  // trace yields zero rows but the vtab must still be discoverable and
  // queryable on the secondary connection.
  int primary_thread_count = -1;
  {
    auto it = tp->ExecuteQuery("SELECT COUNT(*) FROM thread;");
    ASSERT_TRUE(it.Next()) << it.Status().c_message();
    primary_thread_count = static_cast<int>(it.Get(0).long_value);
    ASSERT_FALSE(it.Next());
    ASSERT_OK(it.Status());
  }

  auto conn = tp->CreateConnection();
  ASSERT_NE(conn, nullptr);

  auto it = conn->ExecuteQuery("SELECT COUNT(*) FROM thread;");
  ASSERT_TRUE(it.Next()) << it.Status().c_message();
  ASSERT_EQ(static_cast<int>(it.Get(0).long_value), primary_thread_count);
  ASSERT_FALSE(it.Next());
  ASSERT_OK(it.Status());
}

// Phase 2 iter 5 smoke test: a `CREATE PERFETTO FUNCTION` issued on
// connection-0 is propagated to a freshly-minted secondary connection via
// the staging-area function pool. The secondary connection's engine diffs
// `last_synced_function_version_` against the pool at the start of every
// `Execute` and re-registers any missing entries on its own `sqlite3*`.
TEST(TraceProcessorConnectionTest, DynamicFunctionPropagatesToSecondary) {
  auto tp = TraceProcessor::CreateInstance(Config());
  ASSERT_OK(tp->NotifyEndOfFile());

  // Define a scalar function on the primary (writer) connection.
  {
    auto it = tp->ExecuteQuery(
        "CREATE PERFETTO FUNCTION conn_double(x INT) RETURNS INT "
        "AS SELECT $x * 2;");
    while (it.Next()) {
    }
    ASSERT_OK(it.Status());
  }

  // Verify the writer can call it.
  {
    auto it = tp->ExecuteQuery("SELECT conn_double(21);");
    ASSERT_TRUE(it.Next()) << it.Status().c_message();
    ASSERT_EQ(it.Get(0).long_value, 42);
    ASSERT_OK(it.Status());
  }

  // A secondary connection minted *after* the function was created should
  // pick it up via the function-pool sync at the top of ExecuteUntilLast.
  auto conn = tp->CreateConnection();
  ASSERT_NE(conn, nullptr);
  auto it = conn->ExecuteQuery("SELECT conn_double(7);");
  ASSERT_TRUE(it.Next()) << it.Status().c_message();
  ASSERT_EQ(it.Get(0).long_value, 14);
  ASSERT_FALSE(it.Next());
  ASSERT_OK(it.Status());
}

// Verifies the version-diff mechanism is incremental: a function created
// on connection-0 *after* a secondary connection has been minted (and
// possibly already used for unrelated queries) must still be visible on
// the secondary's next `Execute`. This is the core invariant that makes
// the function pool a "diff at every Execute start" rather than a
// "snapshot at connection-mint time".
TEST(TraceProcessorConnectionTest, DynamicFunctionPickedUpIncrementally) {
  auto tp = TraceProcessor::CreateInstance(Config());
  ASSERT_OK(tp->NotifyEndOfFile());

  // Mint conn-1 *first*, before any dynamic function exists.
  auto conn = tp->CreateConnection();
  ASSERT_NE(conn, nullptr);

  // Run an unrelated query on conn-1 to advance its `last_synced_version_`
  // through one Execute cycle (pool is empty so this is a no-op sync).
  {
    auto it = conn->ExecuteQuery("SELECT 1;");
    ASSERT_TRUE(it.Next()) << it.Status().c_message();
    ASSERT_EQ(it.Get(0).long_value, 1);
    ASSERT_FALSE(it.Next());
    ASSERT_OK(it.Status());
  }

  // Now, on conn-0, create two functions in succession.
  {
    auto it = tp->ExecuteQuery(
        "CREATE PERFETTO FUNCTION conn_inc_a(x INT) RETURNS INT "
        "AS SELECT $x + 100;");
    while (it.Next()) {
    }
    ASSERT_OK(it.Status());
  }
  {
    auto it = tp->ExecuteQuery(
        "CREATE PERFETTO FUNCTION conn_inc_b(x INT) RETURNS INT "
        "AS SELECT $x + 200;");
    while (it.Next()) {
    }
    ASSERT_OK(it.Status());
  }

  // Conn-1's next `Execute` should pick up *both* via the version diff.
  {
    auto it = conn->ExecuteQuery("SELECT conn_inc_a(1) + conn_inc_b(2);");
    ASSERT_TRUE(it.Next()) << it.Status().c_message();
    ASSERT_EQ(it.Get(0).long_value, 1 + 100 + 2 + 200);
    ASSERT_OK(it.Status());
  }

  // And a *third* function added now should also flow through on the
  // subsequent `Execute` (the diff is truly per-Execute, not one-shot).
  {
    auto it = tp->ExecuteQuery(
        "CREATE PERFETTO FUNCTION conn_inc_c(x INT) RETURNS INT "
        "AS SELECT $x + 1000;");
    while (it.Next()) {
    }
    ASSERT_OK(it.Status());
  }
  {
    auto it = conn->ExecuteQuery("SELECT conn_inc_c(5);");
    ASSERT_TRUE(it.Next()) << it.Status().c_message();
    ASSERT_EQ(it.Get(0).long_value, 1005);
    ASSERT_OK(it.Status());
  }
}

}  // namespace
}  // namespace perfetto::trace_processor
