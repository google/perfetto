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

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/iterator.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/base/test/status_matchers.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

using Conn = TraceProcessor::Connection;
using ModuleSpec = std::pair<std::string, std::string>;

// Shared fixture for the secondary-connection test suite. All tests
// run against a fresh `TraceProcessor` with `NotifyEndOfFile` already
// called (the multi-conn API is post-EOF only). Helpers below collapse
// the recurring patterns (drain-and-assert-ok, single-scalar SELECT,
// SqlPackage build+register, threaded worker fan-out) so each test
// body only contains what makes it unique.
class TraceProcessorConnectionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    tp_ = TraceProcessor::CreateInstance(Config());
    ASSERT_OK(tp_->NotifyEndOfFile());
  }

  // Drives the iterator to completion and returns its terminal status.
  static base::Status Drain(Iterator& it) {
    while (it.Next()) {
    }
    return it.Status();
  }

  // Runs `sql` and returns the terminal status. Use with `ASSERT_OK`/
  // `EXPECT_OK` / `EXPECT_FALSE(_.ok())` at the call site.
  base::Status Exec(const std::string& sql) {
    auto it = tp_->ExecuteQuery(sql);
    return Drain(it);
  }
  static base::Status ExecOn(Conn* conn, const std::string& sql) {
    auto it = conn->ExecuteQuery(sql);
    return Drain(it);
  }

  // Runs `sql` and returns the int value in column 0 of the first row.
  // Asserts (non-fatally) that exactly one row was returned and the
  // terminal status was OK; if the query fails or has no rows the
  // returned value is undefined and the test will be flagged failed.
  int64_t QueryLong(const std::string& sql) {
    return QueryLongOn(tp_.get(), sql);
  }
  template <typename T>
  static int64_t QueryLongOn(T* exec, const std::string& sql) {
    auto it = exec->ExecuteQuery(sql);
    EXPECT_TRUE(it.Next()) << it.Status().c_message();
    int64_t v = it.Get(0).long_value;
    EXPECT_OK(Drain(it));
    return v;
  }

  // Mints a secondary connection. Caller owns the returned handle.
  std::unique_ptr<Conn> MintConn() {
    auto c = tp_->CreateConnection();
    EXPECT_NE(c, nullptr);
    return c;
  }

  // Registers a `SqlPackage` named `name` with `modules` as
  // (module_name, body) pairs. Asserts (fatally) the registration
  // succeeded.
  void RegisterPackage(const std::string& name,
                       std::vector<ModuleSpec> modules) {
    SqlPackage pkg;
    pkg.name = name;
    for (auto& m : modules) {
      pkg.modules.emplace_back(std::move(m.first), std::move(m.second));
    }
    ASSERT_OK(tp_->RegisterSqlPackage(pkg));
  }

  // Spawns a thread per connection in `conns`, each running `body`
  // with its own connection moved in. Returns the number of times
  // `body` reported a failure via the supplied counter.
  template <typename Body>
  static int RunOnThreads(std::vector<std::unique_ptr<Conn>> conns,
                          Body body) {
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    for (size_t i = 0; i < conns.size(); ++i) {
      threads.emplace_back(
          [&errors, body, conn = std::move(conns[i]), i]() mutable {
            body(std::move(conn), i, errors);
          });
    }
    for (auto& t : threads) {
      t.join();
    }
    return errors.load();
  }

  std::unique_ptr<TraceProcessor> tp_;
};

// Smoke test for `TraceProcessor::CreateConnection` and the secondary
// `Connection` it returns.
TEST_F(TraceProcessorConnectionTest, SecondaryConnectionExecutesTrivialQuery) {
  auto conn = MintConn();
  // Trivial query that touches no tables: exercises the per-connection
  // SQLite handle and the Iterator wiring without depending on any
  // schema or vtab/function being replicated to the secondary engine.
  EXPECT_EQ(QueryLongOn(conn.get(), "SELECT 1"), 1);
}

TEST_F(TraceProcessorConnectionTest, SecondaryConnectionSeesPrimarySchema) {
  // Plain SQL table on the writer lives in `main` and propagates to
  // other connections via the shared memdb store.
  ASSERT_OK(Exec("CREATE TABLE conn_test_table(id INTEGER, val TEXT);"));
  ASSERT_OK(Exec("INSERT INTO conn_test_table VALUES(7, 'hello');"));

  auto conn = MintConn();
  auto it = conn->ExecuteQuery(
      "SELECT id, val FROM conn_test_table ORDER BY id;");
  ASSERT_TRUE(it.Next()) << it.Status().c_message();
  EXPECT_EQ(it.Get(0).long_value, 7);
  EXPECT_STREQ(it.Get(1).string_value, "hello");
  EXPECT_OK(Drain(it));
}

TEST_F(TraceProcessorConnectionTest, MultipleConnectionsCoexist) {
  auto conn_a = MintConn();
  auto conn_b = MintConn();
  EXPECT_EQ(QueryLongOn(conn_a.get(), "SELECT 1"), 1);
  EXPECT_EQ(QueryLongOn(conn_b.get(), "SELECT 2"), 2);
}

// A successful `INCLUDE PERFETTO MODULE` issued on the default
// connection promotes its CREATE statements to `main`, so a freshly-
// minted secondary connection (sharing memdb) sees the new objects.
TEST_F(TraceProcessorConnectionTest, IncludePromotesObjectsToOtherConnections) {
  RegisterPackage("include_promote_test",
                  {{"include_promote_test.tables",
                    "CREATE TABLE include_promote_t(id INTEGER, name TEXT);\n"
                    "INSERT INTO include_promote_t VALUES (1, 'alpha'),"
                    " (2, 'beta');\n"}});
  ASSERT_OK(Exec("INCLUDE PERFETTO MODULE include_promote_test.tables;"));

  // Secondary minted *after* the include sees both rows because the
  // savepoint released the DDL onto `main`.
  auto conn = MintConn();
  auto it = conn->ExecuteQuery(
      "SELECT id, name FROM include_promote_t ORDER BY id;");
  ASSERT_TRUE(it.Next()) << it.Status().c_message();
  EXPECT_EQ(it.Get(0).long_value, 1);
  EXPECT_STREQ(it.Get(1).string_value, "alpha");
  ASSERT_TRUE(it.Next()) << it.Status().c_message();
  EXPECT_EQ(it.Get(0).long_value, 2);
  EXPECT_STREQ(it.Get(1).string_value, "beta");
  EXPECT_OK(Drain(it));
}

// Failed-include atomicity: a module whose body has a bad statement at
// the end must (a) return an error and (b) leave no trace of any
// CREATE statements that ran before the bad one.
TEST_F(TraceProcessorConnectionTest, FailedIncludeLeavesNoTrace) {
  RegisterPackage(
      "include_fail_test",
      {{"include_fail_test.broken",
        "CREATE TABLE include_fail_t(id INTEGER);\n"
        "INSERT INTO include_fail_t VALUES (42);\n"
        // Fails — references a column that doesn't exist.
        "SELECT no_such_column FROM include_fail_t;\n"}});
  EXPECT_FALSE(Exec("INCLUDE PERFETTO MODULE include_fail_test.broken;").ok());

  // Neither the writer nor a secondary connection should see the
  // partially-created table.
  auto conn = MintConn();
  for (auto* exec : {static_cast<void*>(tp_.get()),
                     static_cast<void*>(conn.get())}) {
    int64_t cnt = exec == tp_.get()
                      ? QueryLong("SELECT count(*) FROM sqlite_master "
                                  "WHERE name = 'include_fail_t';")
                      : QueryLongOn(conn.get(),
                                    "SELECT count(*) FROM sqlite_master "
                                    "WHERE name = 'include_fail_t';");
    EXPECT_EQ(cnt, 0);
  }
}

// Two distinct successful includes from the writer should both be
// visible on a later-minted secondary connection.
TEST_F(TraceProcessorConnectionTest,
       SequentialIncludesPromoteToOtherConnection) {
  RegisterPackage(
      "include_seq_test",
      {{"include_seq_test.first",
        "CREATE TABLE include_seq_a(x INTEGER);\n"
        "INSERT INTO include_seq_a VALUES (1);\n"},
       {"include_seq_test.second",
        "CREATE TABLE include_seq_b(y INTEGER);\n"
        "INSERT INTO include_seq_b VALUES (2);\n"}});
  ASSERT_OK(Exec("INCLUDE PERFETTO MODULE include_seq_test.first;"));
  ASSERT_OK(Exec("INCLUDE PERFETTO MODULE include_seq_test.second;"));

  auto conn = MintConn();
  EXPECT_EQ(QueryLongOn(conn.get(),
                        "SELECT (SELECT x FROM include_seq_a) + "
                        "(SELECT y FROM include_seq_b);"),
            3);
}

// Every top-level `Execute(sql)` opens a SAVEPOINT
// (`perfetto_execute_<n>`) so that if a later statement in a multi-
// statement SQL string fails, earlier side-effects are rolled back.
TEST_F(TraceProcessorConnectionTest,
       MultiStatementExecuteRollsBackOnFailure) {
  // Second CREATE collides with the first; the whole Execute must
  // roll back so the table never lands.
  EXPECT_FALSE(Exec("CREATE TABLE multistmt_t (x INT); "
                    "CREATE TABLE multistmt_t (y INT);")
                   .ok());

  // Neither writer nor a fresh secondary should see the table.
  auto conn = MintConn();
  EXPECT_EQ(QueryLong("SELECT count(*) FROM sqlite_master "
                      "WHERE name = 'multistmt_t';"),
            0);
  EXPECT_EQ(QueryLongOn(conn.get(),
                        "SELECT count(*) FROM sqlite_master "
                        "WHERE name = 'multistmt_t';"),
            0);
}

// Positive control for the execute-savepoint wrap: two successful
// CREATEs in a single Execute land normally and are visible to a
// secondary connection.
TEST_F(TraceProcessorConnectionTest, MultiStatementExecuteCommitsOnSuccess) {
  ASSERT_OK(Exec("CREATE TABLE multistmt_ok_a (x INT); "
                 "CREATE TABLE multistmt_ok_b (y INT); "
                 "INSERT INTO multistmt_ok_a VALUES (7); "
                 "INSERT INTO multistmt_ok_b VALUES (11);"));

  auto conn = MintConn();
  EXPECT_EQ(QueryLongOn(conn.get(),
                        "SELECT (SELECT x FROM multistmt_ok_a) + "
                        "(SELECT y FROM multistmt_ok_b);"),
            18);
}

// `CREATE PERFETTO TABLE` installs a dataframe-backed virtual table.
// The secondary connection must be able to `SELECT` from it and
// observe the same rows, exercising:
//  - dataframe vtab module registration on the secondary engine
//  - cross-connection vtab-state publishing via `PerfettoSqlDatabase`
//    (writer publishes its committed `PerVtabState` on every
//    `OnCommit`)
//  - cold xConnect resolution on the secondary consulting staging
//    via `DataframeModule::Context::ResolveMissingStateOnConnect`
//  - re-resolution at cursor creation time (`DataframeModule::Filter`
//    / `BestIndex` look up the State from staging rather than caching
//    it).
TEST_F(TraceProcessorConnectionTest,
       SecondaryConnectionReadsDataframeVtabFromPrimary) {
  ASSERT_OK(Exec("CREATE PERFETTO TABLE conn_df_test AS "
                 "SELECT 1 AS id, 'first' AS name UNION ALL "
                 "SELECT 2 AS id, 'second' AS name UNION ALL "
                 "SELECT 3 AS id, 'third' AS name;"));

  // Writer sees the rows.
  EXPECT_EQ(QueryLong("SELECT count(*) FROM conn_df_test;"), 3);

  // Secondary sees them too with the right contents.
  auto conn = MintConn();
  auto it =
      conn->ExecuteQuery("SELECT id, name FROM conn_df_test ORDER BY id;");
  for (int64_t i = 1; i <= 3; ++i) {
    ASSERT_TRUE(it.Next()) << it.Status().c_message();
    EXPECT_EQ(it.Get(0).long_value, i);
  }
  EXPECT_OK(Drain(it));
}

// Static dataframe-backed tables (`thread`, `process`, …) are
// registered via `RegisterStaticTable` during engine init; they should
// be discoverable and queryable on secondary connections too, even on
// an empty trace (zero rows is a valid answer).
TEST_F(TraceProcessorConnectionTest,
       SecondaryConnectionReadsStaticDataframeTable) {
  int64_t primary_count = QueryLong("SELECT COUNT(*) FROM thread;");
  auto conn = MintConn();
  EXPECT_EQ(QueryLongOn(conn.get(), "SELECT COUNT(*) FROM thread;"),
            primary_count);
}

// A `CREATE PERFETTO FUNCTION` issued on the writer is propagated to a
// freshly-minted secondary via the staging-area function pool. The
// secondary's engine diffs `last_synced_function_version_` against the
// pool at the start of every `Execute` and re-registers any missing
// entries on its own `sqlite3*`.
TEST_F(TraceProcessorConnectionTest, DynamicFunctionPropagatesToSecondary) {
  ASSERT_OK(Exec("CREATE PERFETTO FUNCTION conn_double(x INT) RETURNS INT "
                 "AS SELECT $x * 2;"));
  EXPECT_EQ(QueryLong("SELECT conn_double(21);"), 42);

  auto conn = MintConn();
  EXPECT_EQ(QueryLongOn(conn.get(), "SELECT conn_double(7);"), 14);
}

// Version-diff is incremental: a function created on the writer
// *after* a secondary has been minted (and possibly already used) must
// still be visible on the secondary's next `Execute`. The pool diff is
// per-Execute, not one-shot at connection-mint time.
TEST_F(TraceProcessorConnectionTest, DynamicFunctionPickedUpIncrementally) {
  // Mint conn-1 first, before any dynamic function exists. Run an
  // unrelated query so its `last_synced_version_` advances through one
  // (empty) Execute cycle.
  auto conn = MintConn();
  EXPECT_EQ(QueryLongOn(conn.get(), "SELECT 1;"), 1);

  // Now create two functions in succession on the writer.
  ASSERT_OK(Exec("CREATE PERFETTO FUNCTION conn_inc_a(x INT) RETURNS INT "
                 "AS SELECT $x + 100;"));
  ASSERT_OK(Exec("CREATE PERFETTO FUNCTION conn_inc_b(x INT) RETURNS INT "
                 "AS SELECT $x + 200;"));

  // conn-1's next Execute picks up *both* via the version diff.
  EXPECT_EQ(QueryLongOn(conn.get(),
                        "SELECT conn_inc_a(1) + conn_inc_b(2);"),
            1 + 100 + 2 + 200);

  // A *third* function added now should also flow through on the
  // subsequent Execute (the diff is truly per-Execute, not one-shot).
  ASSERT_OK(Exec("CREATE PERFETTO FUNCTION conn_inc_c(x INT) RETURNS INT "
                 "AS SELECT $x + 1000;"));
  EXPECT_EQ(QueryLongOn(conn.get(), "SELECT conn_inc_c(5);"), 1005);
}

// Two secondary connections, each owned and used exclusively by its
// own thread, run a tight loop of trivial queries concurrently.
// Connections are thread-compatible (not thread-safe), so each thread
// takes ownership of its own `Connection`. Asserts no crashes / data
// races / wrong results — baseline for subsequent stress work.
TEST_F(TraceProcessorConnectionTest, ConcurrentReadersDoNotCrash) {
  std::vector<std::unique_ptr<Conn>> conns;
  conns.push_back(MintConn());
  conns.push_back(MintConn());

  constexpr int kIters = 50;
  int errs = RunOnThreads(
      std::move(conns),
      [](std::unique_ptr<Conn> conn, size_t tid, std::atomic<int>& errors) {
        const int64_t expected = static_cast<int64_t>(tid) + 1;
        for (int i = 0; i < kIters; ++i) {
          int64_t v = QueryLongOn(conn.get(),
                                  "SELECT " + std::to_string(expected));
          if (v != expected) {
            errors.fetch_add(1, std::memory_order_relaxed);
            return;
          }
        }
      });
  EXPECT_EQ(errs, 0);
}

// Single-thread sanity for the include-lock plumbing. The per-module
// recursive mutex must be acquired/released for every
// `INCLUDE PERFETTO MODULE <name>` (and every individual module from
// wildcard expansion) without deadlock. Re-issuing an include of an
// already-included module short-circuits before re-acquiring the
// lock.
TEST_F(TraceProcessorConnectionTest, IncludeLockAcquisitionDoesNotDeadlock) {
  RegisterPackage(
      "include_lock_test",
      {{"include_lock_test.first",
        "CREATE TABLE include_lock_first(x INTEGER);\n"},
       {"include_lock_test.second",
        "CREATE TABLE include_lock_second(y INTEGER);\n"}});

  // Single-key, then wildcard, then re-issue (no-op).
  ASSERT_OK(Exec("INCLUDE PERFETTO MODULE include_lock_test.first;"));
  ASSERT_OK(Exec("INCLUDE PERFETTO MODULE include_lock_test.*;"));
  ASSERT_OK(Exec("INCLUDE PERFETTO MODULE include_lock_test.first;"));

  auto conn = MintConn();
  EXPECT_EQ(QueryLongOn(conn.get(),
                        "SELECT count(*) FROM sqlite_master WHERE name IN "
                        "('include_lock_first', 'include_lock_second');"),
            2);
}

// A `RegisterSqlPackage` on the writer is observable on a freshly-
// minted secondary via `INCLUDE PERFETTO MODULE`. The secondary's
// `packages_` is populated by `SyncPackagesFromPool` at the top of
// every top-level `Execute`.
TEST_F(TraceProcessorConnectionTest,
       IncludeOnSecondaryConnectionWorksAfterPackageRegister) {
  RegisterPackage(
      "pkg_propagate_test",
      {{"pkg_propagate_test.tables",
        "CREATE TABLE pkg_propagate_t(id INTEGER, label TEXT);\n"
        "INSERT INTO pkg_propagate_t VALUES (1, 'foo'), (2, 'bar');\n"}});

  // Mint AFTER registration so the include exercises pool-sync.
  auto conn = MintConn();
  ASSERT_OK(ExecOn(conn.get(),
                   "INCLUDE PERFETTO MODULE pkg_propagate_test.tables;"));
  EXPECT_EQ(QueryLongOn(conn.get(),
                        "SELECT count(*) FROM pkg_propagate_t;"),
            2);
}

// The package-pool diff is incremental: entries appended after the
// writer registers them flow to subsequently-minted secondaries. The
// TP-level `RegisterSqlPackage` is gated on `non_default_connection_
// count_ == 0` so this test interleaves register-and-mint cycles.
// Round 1: register pkg_a, mint+use+drop a secondary. Round 2:
// register pkg_b, mint a fresh secondary; its first sync picks up
// *both* pool entries — i.e. the pool retains pkg_a across the second
// append, the diff is purely additive.
TEST_F(TraceProcessorConnectionTest,
       IncrementalPackageRegistrationFlowsToSecondary) {
  RegisterPackage("pkg_inc_a",
                  {{"pkg_inc_a.tables",
                    "CREATE TABLE pkg_inc_a_t(x INTEGER);\n"
                    "INSERT INTO pkg_inc_a_t VALUES (101);\n"}});

  // Round 1: secondary picks up pkg_a, includes it (promoting onto
  // `main`), then is dropped.
  {
    auto conn1 = MintConn();
    ASSERT_OK(ExecOn(conn1.get(),
                     "INCLUDE PERFETTO MODULE pkg_inc_a.tables;"));
    EXPECT_EQ(QueryLongOn(conn1.get(), "SELECT x FROM pkg_inc_a_t;"), 101);
  }
  // conn1 destructor decrements non_default_connection_count_ to 0.

  // Round 2: register pkg_b; fresh secondary's first sync picks up
  // both entries on its engine.
  RegisterPackage("pkg_inc_b",
                  {{"pkg_inc_b.tables",
                    "CREATE TABLE pkg_inc_b_t(y INTEGER);\n"
                    "INSERT INTO pkg_inc_b_t VALUES (202);\n"}});

  auto conn2 = MintConn();
  ASSERT_OK(ExecOn(conn2.get(),
                   "INCLUDE PERFETTO MODULE pkg_inc_b.tables;"));
  // pkg_a's table was promoted in round 1; pkg_b's just now. Both
  // reachable from conn2.
  EXPECT_EQ(QueryLongOn(conn2.get(),
                        "SELECT (SELECT x FROM pkg_inc_a_t) + "
                        "(SELECT y FROM pkg_inc_b_t);"),
            303);
}

// End-to-end concurrency through the per-module include lock plus
// cross-connection package propagation. Writer pre-includes a module;
// two secondary threads each then INCLUDE the same module
// (short-circuited via `IsModuleIncluded`) and SELECT from its table.
// Validates: no deadlocks under contention, correct package-pool sync,
// and `IsModuleIncluded` short-circuit hits across connections.
TEST_F(TraceProcessorConnectionTest,
       ConcurrentIncludesOfSameModuleSerialise) {
  RegisterPackage(
      "concurrent_include_test",
      {{"concurrent_include_test.tables",
        "CREATE TABLE concurrent_include_t(id INTEGER, val TEXT);\n"
        "INSERT INTO concurrent_include_t VALUES (1, 'a'), (2, 'b');\n"}});
  ASSERT_OK(Exec("INCLUDE PERFETTO MODULE concurrent_include_test.tables;"));

  std::vector<std::unique_ptr<Conn>> conns;
  conns.push_back(MintConn());
  conns.push_back(MintConn());

  int errs = RunOnThreads(
      std::move(conns),
      [](std::unique_ptr<Conn> conn, size_t, std::atomic<int>& errors) {
        if (!ExecOn(conn.get(),
                    "INCLUDE PERFETTO MODULE concurrent_include_test.tables;")
                 .ok()) {
          errors.fetch_add(1);
          return;
        }
        if (QueryLongOn(conn.get(),
                        "SELECT count(*) FROM concurrent_include_t;") != 2) {
          errors.fetch_add(1);
        }
      });
  EXPECT_EQ(errs, 0);
}

// Two secondary connections concurrently INCLUDE the *same* module
// that no other connection has pre-included. Both racers run the body
// (CREATE TABLE + INSERT) inside per-connection temp schemas, then
// re-issue them as DDL on shared `main`. The shared-cache schema lock
// returns SQLITE_LOCKED to whichever transaction is second; the
// transparent BUSY/LOCKED retry middleware makes that invisible — the
// second writer waits, retries, and either acquires the lock and
// re-issues its DDL (which the per-module `IsModuleIncluded` short-
// circuit prevents from re-creating) or finds the module already
// promoted and short-circuits the body entirely.
TEST_F(TraceProcessorConnectionTest,
       ConcurrentIncludesUnderSharedCacheNowSucceeds) {
  RegisterPackage(
      "concurrent_include_lift_test",
      {{"concurrent_include_lift_test.tables",
        "CREATE TABLE concurrent_include_lift_t(id INTEGER, val TEXT);\n"
        "INSERT INTO concurrent_include_lift_t VALUES (1, 'a'),"
        " (2, 'b');\n"}});

  std::vector<std::unique_ptr<Conn>> conns;
  conns.push_back(MintConn());
  conns.push_back(MintConn());

  int errs = RunOnThreads(
      std::move(conns),
      [](std::unique_ptr<Conn> conn, size_t, std::atomic<int>& errors) {
        if (!ExecOn(conn.get(),
                    "INCLUDE PERFETTO MODULE "
                    "concurrent_include_lift_test.tables;")
                 .ok()) {
          errors.fetch_add(1);
          return;
        }
        if (QueryLongOn(conn.get(),
                        "SELECT count(*) FROM concurrent_include_lift_t;") !=
            2) {
          errors.fetch_add(1);
        }
      });
  EXPECT_EQ(errs, 0);
}

// SqlStats stress test. Each `ExecuteQuery` records `RecordQueryBegin`
// + `RecordQueryFirstNext` + `RecordQueryEnd` against the parent's
// shared `TraceStorage::SqlStats`. Multiple connections running on
// their own threads race on the same `std::deque`s. Pre-fix, ASan
// caught a container-overflow within a few iterations; post-fix the
// run is clean and `SELECT count(*) FROM sqlstats` returns the bounded
// log size (`kMaxLogEntries == 100`).
TEST_F(TraceProcessorConnectionTest, ConcurrentRecordingIntoSqlStats) {
  constexpr int kThreads = 4;
  constexpr int kItersPerThread = 100;
  std::vector<std::unique_ptr<Conn>> conns;
  for (int i = 0; i < kThreads; ++i) {
    conns.push_back(MintConn());
  }
  int errs = RunOnThreads(
      std::move(conns),
      [](std::unique_ptr<Conn> conn, size_t tid, std::atomic<int>& errors) {
        for (int i = 0; i < kItersPerThread; ++i) {
          // Distinct query per (thread, iter) so the SqlStats deque
          // observes a steady stream of writes (no dedup).
          if (!ExecOn(conn.get(),
                      "SELECT " + std::to_string(
                                      static_cast<int>(tid) * kItersPerThread +
                                      i))
                   .ok()) {
            errors.fetch_add(1);
            return;
          }
        }
      });
  EXPECT_EQ(errs, 0);

  // SqlStats caps at kMaxLogEntries = 100.
  int64_t logged = QueryLong("SELECT count(*) FROM sqlstats;");
  EXPECT_GT(logged, 0);
  EXPECT_LE(logged, 100);
}

// `TraceProcessorImpl::CreateConnection` flips `StringPool::Enable
// ThreadSafetyForMultiConnection` on the shared `TraceStorage`-owned
// pool before returning the secondary. After the flip, concurrent
// `InternString` calls from any thread must be serialised by the
// pool's internal mutex. This test populates many strings via the
// writer, then has four secondary connections concurrently SELECT
// from a column that returns them. Reads exercise `Get(Id)` (the
// documented lock-free fast-path) on every row. Verifies the
// `CreateConnection`-side wiring is correct; the deeper "many threads
// concurrently `InternString`" stress runs in the StringPool unit
// tests.
TEST_F(TraceProcessorConnectionTest,
       ConcurrentInternFromMultipleConnections) {
  constexpr int kRowsPerTable = 32;
  constexpr int kThreads = 4;
  constexpr int kReadsPerThread = 200;

  // Pre-populate a shared table on the writer; each row a unique
  // string. CREATE PERFETTO TABLE interns every column value through
  // `RuntimeDataframeBuilder::AddRow` — so the pool now holds
  // kRowsPerTable entries reachable by any subsequent SELECT.
  std::string create =
      "CREATE PERFETTO TABLE stress_strings AS WITH src(s) AS (VALUES";
  for (int i = 0; i < kRowsPerTable; ++i) {
    create += (i ? ",('" : "('") + std::string("preload_value_") +
              std::to_string(i) + "')";
  }
  create += ") SELECT s FROM src;";
  ASSERT_OK(Exec(create));

  std::vector<std::unique_ptr<Conn>> conns;
  for (int i = 0; i < kThreads; ++i) {
    conns.push_back(MintConn());
  }
  int errs = RunOnThreads(
      std::move(conns),
      [](std::unique_ptr<Conn> conn, size_t, std::atomic<int>& errors) {
        for (int i = 0; i < kReadsPerThread; ++i) {
          if (QueryLongOn(conn.get(),
                          "SELECT count(*) FROM stress_strings;") !=
              kRowsPerTable) {
            errors.fetch_add(1);
            return;
          }
        }
      });
  EXPECT_EQ(errs, 0);
}

// Companion to the read-side stress: secondary connections concurrent-
// ly issue equality-filtered SELECTs on the string column. Every
// comparison goes through `Get(Id)` on the pool (lock-free hot-path)
// plus a hash-into-`string_index_` lookup via `GetId` (lock-taking)
// for every literal in the WHERE clause. With the
// `EnableThreadSafetyForMultiConnection` flip, these reads run race-
// free; without it the parallel `GetId` callers would observe torn
// reads in the FlatHashMap.
TEST_F(TraceProcessorConnectionTest,
       InternedStringMatchesAcrossConnections) {
  constexpr int kThreads = 4;
  constexpr int kSharedStrings = 32;
  constexpr int kReadsPerThread = 100;

  std::vector<std::string> shared_strings;
  for (int i = 0; i < kSharedStrings; ++i) {
    shared_strings.push_back("shared_value_" + std::to_string(i));
  }

  std::string create =
      "CREATE PERFETTO TABLE intern_match AS WITH src(s) AS (VALUES";
  for (size_t i = 0; i < shared_strings.size(); ++i) {
    create += (i ? ",('" : "('") + shared_strings[i] + "')";
  }
  create += ") SELECT s FROM src;";
  ASSERT_OK(Exec(create));

  std::vector<std::unique_ptr<Conn>> conns;
  for (int i = 0; i < kThreads; ++i) {
    conns.push_back(MintConn());
  }
  int errs = RunOnThreads(
      std::move(conns),
      [&shared_strings](std::unique_ptr<Conn> conn, size_t tid,
                        std::atomic<int>& errors) {
        for (int i = 0; i < kReadsPerThread; ++i) {
          const std::string& target =
              shared_strings[(static_cast<size_t>(i) + tid) %
                             shared_strings.size()];
          if (QueryLongOn(conn.get(),
                          "SELECT count(*) FROM intern_match WHERE s = '" +
                              target + "';") != 1) {
            errors.fetch_add(1);
            return;
          }
        }
      });
  EXPECT_EQ(errs, 0);
}

// SQLite returns SQLITE_SCHEMA from `sqlite3_step` (and sometimes
// `sqlite3_prepare_v2`) when another connection has bumped the schema
// cookie since the statement was prepared. The fix is to finalise and
// re-prepare from the original SqlSource — see
// `SqliteEngine::PreparedStatement::ReprepareFromSource`. This test
// interleaves DDL on the writer with SELECTs on a secondary; the
// transparent retry middleware must absorb the SCHEMA error.
TEST_F(TraceProcessorConnectionTest, SchemaRetryRePreparesOnSchemaChange) {
  ASSERT_OK(Exec("CREATE TABLE schema_retry_t(v INTEGER); "
                 "INSERT INTO schema_retry_t VALUES (42);"));
  auto conn = MintConn();

  // Bump the schema cookie. Next prepare on `conn` would otherwise
  // surface SQLITE_SCHEMA; the retry middleware re-prepares
  // transparently.
  ASSERT_OK(Exec("CREATE TABLE schema_retry_other(x INTEGER);"));
  EXPECT_EQ(QueryLongOn(conn.get(), "SELECT v FROM schema_retry_t;"), 42);

  // Repeat: another bump, another SELECT.
  ASSERT_OK(Exec("CREATE TABLE schema_retry_more(y INTEGER);"));
  EXPECT_EQ(QueryLongOn(conn.get(), "SELECT v + 1 FROM schema_retry_t;"), 43);
}

// Stress: a writer thread does N×(CREATE + DROP) DDL while a reader
// on a sibling connection does M×(SELECT 1). Every CREATE/DROP commit
// bumps the schema cookie; without the SCHEMA retry the reader would
// surface "database schema has changed" intermittently. With it the
// reader sees zero errors.
TEST_F(TraceProcessorConnectionTest, ConcurrentDDLDoesNotBreakReaders) {
  auto reader_conn = MintConn();
  constexpr int kDdlIters = 100;
  constexpr int kReadIters = 200;
  std::atomic<int> errors{0};

  std::thread writer([&] {
    for (int i = 0; i < kDdlIters; ++i) {
      const std::string n = std::to_string(i);
      if (!Exec("CREATE TABLE schema_stress_t" + n + "(v INTEGER);").ok() ||
          !Exec("DROP TABLE schema_stress_t" + n + ";").ok()) {
        errors.fetch_add(1);
        return;
      }
    }
  });
  std::thread reader([&] {
    for (int i = 0; i < kReadIters; ++i) {
      if (QueryLongOn(reader_conn.get(), "SELECT 1;") != 1) {
        errors.fetch_add(1);
        return;
      }
    }
  });
  writer.join();
  reader.join();
  EXPECT_EQ(errors.load(), 0);
}

}  // namespace
}  // namespace perfetto::trace_processor
