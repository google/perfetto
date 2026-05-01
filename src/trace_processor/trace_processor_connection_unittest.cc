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
  static int RunOnThreads(std::vector<std::unique_ptr<Conn>> conns, Body body) {
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    for (size_t i = 0; i < conns.size(); ++i) {
      threads.emplace_back([&errors, body, conn = std::move(conns[i]),
                            i]() mutable { body(std::move(conn), i, errors); });
    }
    for (auto& t : threads) {
      t.join();
    }
    return errors.load();
  }

  std::unique_ptr<TraceProcessor> tp_;
};

TEST_F(TraceProcessorConnectionTest, SecondaryConnectionExecutesTrivialQuery) {
  auto conn = MintConn();
  EXPECT_EQ(QueryLongOn(conn.get(), "SELECT 1"), 1);
}

// Plain SQL DDL on the writer propagates to secondaries via the shared
// memdb store.
TEST_F(TraceProcessorConnectionTest, SecondaryConnectionSeesPrimarySchema) {
  ASSERT_OK(Exec("CREATE TABLE conn_test_table(id INTEGER, val TEXT);"));
  ASSERT_OK(Exec("INSERT INTO conn_test_table VALUES(7, 'hello');"));

  auto conn = MintConn();
  auto it =
      conn->ExecuteQuery("SELECT id, val FROM conn_test_table ORDER BY id;");
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

// Successful INCLUDE on the writer promotes the module's CREATE
// statements to `main`; a secondary minted after sees the new objects.
TEST_F(TraceProcessorConnectionTest, IncludePromotesObjectsToOtherConnections) {
  RegisterPackage("include_promote_test",
                  {{"include_promote_test.tables",
                    "CREATE TABLE include_promote_t(id INTEGER, name TEXT);\n"
                    "INSERT INTO include_promote_t VALUES (1, 'alpha'),"
                    " (2, 'beta');\n"}});
  ASSERT_OK(Exec("INCLUDE PERFETTO MODULE include_promote_test.tables;"));

  auto conn = MintConn();
  auto it =
      conn->ExecuteQuery("SELECT id, name FROM include_promote_t ORDER BY id;");
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
// CREATE that ran before the bad one — on the writer or any secondary.
TEST_F(TraceProcessorConnectionTest, FailedIncludeLeavesNoTrace) {
  RegisterPackage("include_fail_test",
                  {{"include_fail_test.broken",
                    "CREATE TABLE include_fail_t(id INTEGER);\n"
                    "INSERT INTO include_fail_t VALUES (42);\n"
                    "SELECT no_such_column FROM include_fail_t;\n"}});
  EXPECT_FALSE(Exec("INCLUDE PERFETTO MODULE include_fail_test.broken;").ok());

  static constexpr char kCheck[] =
      "SELECT count(*) FROM sqlite_master WHERE name = 'include_fail_t';";
  EXPECT_EQ(QueryLong(kCheck), 0);
  auto conn = MintConn();
  EXPECT_EQ(QueryLongOn(conn.get(), kCheck), 0);
}

// Two successful includes from the writer should both be visible on a
// later-minted secondary.
TEST_F(TraceProcessorConnectionTest,
       SequentialIncludesPromoteToOtherConnection) {
  RegisterPackage("include_seq_test",
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

// Top-level `Execute` is wrapped in a SAVEPOINT so a later-statement
// failure rolls back earlier side-effects.
TEST_F(TraceProcessorConnectionTest, MultiStatementExecuteRollsBackOnFailure) {
  // Second CREATE collides; the whole Execute must roll back.
  EXPECT_FALSE(Exec("CREATE TABLE multistmt_t (x INT); "
                    "CREATE TABLE multistmt_t (y INT);")
                   .ok());
  static constexpr char kCheck[] =
      "SELECT count(*) FROM sqlite_master WHERE name = 'multistmt_t';";
  EXPECT_EQ(QueryLong(kCheck), 0);
  auto conn = MintConn();
  EXPECT_EQ(QueryLongOn(conn.get(), kCheck), 0);
}

// Positive control for the SAVEPOINT wrap.
TEST_F(TraceProcessorConnectionTest, MultiStatementExecuteCommitsOnSuccess) {
  ASSERT_OK(
      Exec("CREATE TABLE multistmt_ok_a (x INT); "
           "CREATE TABLE multistmt_ok_b (y INT); "
           "INSERT INTO multistmt_ok_a VALUES (7); "
           "INSERT INTO multistmt_ok_b VALUES (11);"));

  auto conn = MintConn();
  EXPECT_EQ(QueryLongOn(conn.get(),
                        "SELECT (SELECT x FROM multistmt_ok_a) + "
                        "(SELECT y FROM multistmt_ok_b);"),
            18);
}

// `CREATE PERFETTO TABLE` on the writer installs a dataframe-backed
// vtab; secondaries must be able to SELECT from it. Exercises
// cross-conn vtab-state publishing on writer commit, cold-xConnect
// resolution from the database state map, and re-resolution at cursor
// creation time.
TEST_F(TraceProcessorConnectionTest,
       SecondaryConnectionReadsDataframeVtabFromPrimary) {
  ASSERT_OK(
      Exec("CREATE PERFETTO TABLE conn_df_test AS "
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

// `CREATE PERFETTO FUNCTION` on the writer flows to secondaries via
// the database's function pool (each secondary diffs at the top of
// every `Execute`).
TEST_F(TraceProcessorConnectionTest, DynamicFunctionPropagatesToSecondary) {
  ASSERT_OK(
      Exec("CREATE PERFETTO FUNCTION conn_double(x INT) RETURNS INT "
           "AS SELECT $x * 2;"));
  EXPECT_EQ(QueryLong("SELECT conn_double(21);"), 42);

  auto conn = MintConn();
  EXPECT_EQ(QueryLongOn(conn.get(), "SELECT conn_double(7);"), 14);
}

// The pool diff is per-Execute, not one-shot at mint time: functions
// added after a secondary exists still flow through on its next
// Execute.
TEST_F(TraceProcessorConnectionTest, DynamicFunctionPickedUpIncrementally) {
  auto conn = MintConn();
  EXPECT_EQ(QueryLongOn(conn.get(), "SELECT 1;"), 1);  // empty sync

  ASSERT_OK(
      Exec("CREATE PERFETTO FUNCTION conn_inc_a(x INT) RETURNS INT "
           "AS SELECT $x + 100;"));
  ASSERT_OK(
      Exec("CREATE PERFETTO FUNCTION conn_inc_b(x INT) RETURNS INT "
           "AS SELECT $x + 200;"));
  EXPECT_EQ(QueryLongOn(conn.get(), "SELECT conn_inc_a(1) + conn_inc_b(2);"),
            1 + 100 + 2 + 200);

  // A third function added now flows through on the next Execute too.
  ASSERT_OK(
      Exec("CREATE PERFETTO FUNCTION conn_inc_c(x INT) RETURNS INT "
           "AS SELECT $x + 1000;"));
  EXPECT_EQ(QueryLongOn(conn.get(), "SELECT conn_inc_c(5);"), 1005);
}

// Two secondary connections each on their own thread run a tight loop
// of trivial queries concurrently. Asserts no crashes / data races /
// wrong results.
TEST_F(TraceProcessorConnectionTest, ConcurrentReadersDoNotCrash) {
  std::vector<std::unique_ptr<Conn>> conns;
  conns.push_back(MintConn());
  conns.push_back(MintConn());

  constexpr int kIters = 50;
  int errs = RunOnThreads(std::move(conns), [](std::unique_ptr<Conn> conn,
                                               size_t tid,
                                               std::atomic<int>& errors) {
    const int64_t expected = static_cast<int64_t>(tid) + 1;
    for (int i = 0; i < kIters; ++i) {
      int64_t v = QueryLongOn(conn.get(), "SELECT " + std::to_string(expected));
      if (v != expected) {
        errors.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
  });
  EXPECT_EQ(errs, 0);
}

// Single-thread sanity for the include-claim plumbing. Single-key,
// wildcard, and re-issue paths all complete without deadlock; the
// re-issue short-circuits via the per-engine `included` flag.
TEST_F(TraceProcessorConnectionTest, IncludeLockAcquisitionDoesNotDeadlock) {
  RegisterPackage("include_lock_test",
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

// `RegisterSqlPackage` on the writer is observable on a fresh
// secondary via `INCLUDE PERFETTO MODULE` (its first `Execute` syncs
// the package pool).
TEST_F(TraceProcessorConnectionTest,
       IncludeOnSecondaryConnectionWorksAfterPackageRegister) {
  RegisterPackage(
      "pkg_propagate_test",
      {{"pkg_propagate_test.tables",
        "CREATE TABLE pkg_propagate_t(id INTEGER, label TEXT);\n"
        "INSERT INTO pkg_propagate_t VALUES (1, 'foo'), (2, 'bar');\n"}});

  // Mint AFTER registration so the include exercises pool-sync.
  auto conn = MintConn();
  ASSERT_OK(
      ExecOn(conn.get(), "INCLUDE PERFETTO MODULE pkg_propagate_test.tables;"));
  EXPECT_EQ(QueryLongOn(conn.get(), "SELECT count(*) FROM pkg_propagate_t;"),
            2);
}

// Package-pool diff is purely additive: a second `RegisterSqlPackage`
// after a secondary has used the first still flows to fresh
// secondaries.
TEST_F(TraceProcessorConnectionTest,
       IncrementalPackageRegistrationFlowsToSecondary) {
  RegisterPackage("pkg_inc_a", {{"pkg_inc_a.tables",
                                 "CREATE TABLE pkg_inc_a_t(x INTEGER);\n"
                                 "INSERT INTO pkg_inc_a_t VALUES (101);\n"}});

  // Round 1: secondary picks up pkg_a, includes it (promoting onto
  // `main`), then is dropped.
  {
    auto conn1 = MintConn();
    ASSERT_OK(ExecOn(conn1.get(), "INCLUDE PERFETTO MODULE pkg_inc_a.tables;"));
    EXPECT_EQ(QueryLongOn(conn1.get(), "SELECT x FROM pkg_inc_a_t;"), 101);
  }
  // conn1 destructor decrements non_default_connection_count_ to 0.

  // Round 2: register pkg_b; fresh secondary's first sync picks up
  // both entries on its engine.
  RegisterPackage("pkg_inc_b", {{"pkg_inc_b.tables",
                                 "CREATE TABLE pkg_inc_b_t(y INTEGER);\n"
                                 "INSERT INTO pkg_inc_b_t VALUES (202);\n"}});

  auto conn2 = MintConn();
  ASSERT_OK(ExecOn(conn2.get(), "INCLUDE PERFETTO MODULE pkg_inc_b.tables;"));
  // pkg_a's table was promoted in round 1; pkg_b's just now. Both
  // reachable from conn2.
  EXPECT_EQ(QueryLongOn(conn2.get(),
                        "SELECT (SELECT x FROM pkg_inc_a_t) + "
                        "(SELECT y FROM pkg_inc_b_t);"),
            303);
}

// Writer pre-includes a module; two secondary threads then re-include
// the same module concurrently. The cross-conn already-included
// short-circuit must hit (no body re-runs, no schema-write conflict).
TEST_F(TraceProcessorConnectionTest, ConcurrentIncludesOfSameModuleSerialise) {
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

// Two secondary connections race on the *same* module that no peer
// has pre-included. The cross-conn claim serialises them; the second
// thread either acquires after the first or short-circuits via the
// already-included flag.
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

  int errs = RunOnThreads(std::move(conns), [](std::unique_ptr<Conn> conn,
                                               size_t,
                                               std::atomic<int>& errors) {
    if (!ExecOn(conn.get(),
                "INCLUDE PERFETTO MODULE "
                "concurrent_include_lift_test.tables;")
             .ok()) {
      errors.fetch_add(1);
      return;
    }
    if (QueryLongOn(conn.get(),
                    "SELECT count(*) FROM concurrent_include_lift_t;") != 2) {
      errors.fetch_add(1);
    }
  });
  EXPECT_EQ(errs, 0);
}

// SqlStats stress: multiple connections each on their own thread
// race on the shared `TraceStorage::SqlStats` deques. Pre-fix, ASan
// caught a container-overflow within a few iters; post-fix clean.
TEST_F(TraceProcessorConnectionTest, ConcurrentRecordingIntoSqlStats) {
  constexpr int kThreads = 4;
  constexpr int kItersPerThread = 100;
  std::vector<std::unique_ptr<Conn>> conns;
  for (int i = 0; i < kThreads; ++i) {
    conns.push_back(MintConn());
  }
  int errs = RunOnThreads(std::move(conns), [](std::unique_ptr<Conn> conn,
                                               size_t tid,
                                               std::atomic<int>& errors) {
    for (int i = 0; i < kItersPerThread; ++i) {
      // Distinct query per (thread, iter) so the SqlStats deque
      // observes a steady stream of writes (no dedup).
      if (!ExecOn(conn.get(),
                  "SELECT " + std::to_string(
                                  static_cast<int>(tid) * kItersPerThread + i))
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

// Verifies that `CreateConnection` flips `StringPool` to MT-safe
// mode: secondaries can concurrently SELECT from a string-typed
// column without races. The deeper `InternString` stress lives in
// the StringPool unit tests.
TEST_F(TraceProcessorConnectionTest, ConcurrentInternFromMultipleConnections) {
  constexpr int kRowsPerTable = 32;
  constexpr int kThreads = 4;
  constexpr int kReadsPerThread = 200;

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
          if (QueryLongOn(conn.get(), "SELECT count(*) FROM stress_strings;") !=
              kRowsPerTable) {
            errors.fetch_add(1);
            return;
          }
        }
      });
  EXPECT_EQ(errs, 0);
}

// Companion: secondaries concurrently issue equality-filtered SELECTs
// on a string column. Every comparison hits `GetId` on the shared
// pool (lock-taking); the MT-safety flip makes these race-free.
TEST_F(TraceProcessorConnectionTest, InternedStringMatchesAcrossConnections) {
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

// SQLITE_SCHEMA recovery: a peer's DDL bumps the schema cookie
// between prepare and step on this conn; the retry middleware
// transparently re-prepares.
TEST_F(TraceProcessorConnectionTest, SchemaRetryRePreparesOnSchemaChange) {
  ASSERT_OK(
      Exec("CREATE TABLE schema_retry_t(v INTEGER); "
           "INSERT INTO schema_retry_t VALUES (42);"));
  auto conn = MintConn();

  ASSERT_OK(Exec("CREATE TABLE schema_retry_other(x INTEGER);"));
  EXPECT_EQ(QueryLongOn(conn.get(), "SELECT v FROM schema_retry_t;"), 42);
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
