/*
 * Copyright (C) 2023 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_SQLITE_SQLITE_ENGINE_H_
#define SRC_TRACE_PROCESSOR_SQLITE_SQLITE_ENGINE_H_

#include <sqlite3.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "perfetto/base/status.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/hash.h"
#include "perfetto/ext/base/murmur_hash.h"
#include "src/trace_processor/sqlite/scoped_db.h"
#include "src/trace_processor/sqlite/sql_source.h"

namespace perfetto::trace_processor {

// Transparent retry for SQLite's |SQLITE_BUSY| / |SQLITE_LOCKED|: another
// connection holds the page or schema lock; sleep with exponential backoff
// and try again until |timeout| elapses.
class BusyRetryHelper {
 public:
  static constexpr base::TimeMillis kDefaultTimeout = base::TimeMillis(1000);
  explicit BusyRetryHelper(base::TimeMillis timeout = kDefaultTimeout);

  // Returns true if `sqlite_status` is BUSY/LOCKED and the deadline has
  // not elapsed (sleeps for a backoff interval before returning); false
  // otherwise.
  bool ShouldRetry(int sqlite_status);

  using SleepFn = void(unsigned interval_us);
  void set_sleep_fn_for_testing(SleepFn* fn) { sleep_fn_ = fn; }

 private:
  base::TimeMillis deadline_;
  uint32_t attempt_ = 0;
  SleepFn* sleep_fn_;
};

// Transparent retry for SQLite's |SQLITE_SCHEMA|: the schema cookie has
// been bumped since the statement was prepared. The bytecode is stale and
// the caller must re-prepare from `SqlSource`; this helper only
// administers the budget. Bounded by deadline + a hard attempt count
// cap to guard against pathological "schema bumps every prepare" loops.
class SchemaRetryHelper {
 public:
  static constexpr base::TimeMillis kDefaultTimeout = base::TimeMillis(1000);
  static constexpr uint32_t kMaxAttempts = 100;
  explicit SchemaRetryHelper(base::TimeMillis timeout = kDefaultTimeout);

  bool ShouldRetry(int sqlite_status);
  uint32_t attempt() const { return attempt_; }

 private:
  base::TimeMillis deadline_;
  uint32_t attempt_ = 0;
};

// Wrapper class around SQLite C API.
//
// The goal of this class is to provide a one-stop-shop mechanism to use SQLite.
// Benefits of this include:
// 1) It allows us to add code which intercepts registration of functions
//    and tables and keeps track of this for later lookup.
// 2) Allows easily auditing the SQLite APIs we use making it easy to determine
//    what functionality we rely on.
class SqliteEngine {
 public:
  using Fn = void(sqlite3_context* ctx, int argc, sqlite3_value** argv);
  using AggregateFnStep = void(sqlite3_context* ctx,
                               int argc,
                               sqlite3_value** argv);
  using AggregateFnFinal = void(sqlite3_context* ctx);
  using WindowFnStep = void(sqlite3_context* ctx,
                            int argc,
                            sqlite3_value** argv);
  using WindowFnInverse = void(sqlite3_context* ctx,
                               int argc,
                               sqlite3_value** argv);
  using WindowFnValue = void(sqlite3_context* ctx);
  using WindowFnFinal = void(sqlite3_context* ctx);
  using FnCtxDestructor = void(void*);

  struct PreparedStatement {
   public:
    bool Step();
    bool IsDone() const;
    const char* original_sql() const;
    const char* sql() const;
    const base::Status& status() const { return status_; }
    sqlite3_stmt* sqlite_stmt() const { return stmt_.get(); }

   private:
    friend class SqliteEngine;
    PreparedStatement(ScopedStmt,
                      SqlSource,
                      sqlite3* db,
                      base::TimeMillis retry_timeout);
    // Recovery path for SQLITE_SCHEMA: re-prepare `stmt_` from
    // `sql_source_` against `db_`. Returns the SQLite return code.
    int ReprepareFromSource();

    ScopedStmt stmt_;
    ScopedSqliteString expanded_sql_;
    SqlSource sql_source_;
    base::Status status_ = base::OkStatus();
    sqlite3* db_ = nullptr;  // Non-owning; outlives PreparedStatement.
    base::TimeMillis retry_timeout_;
    // True once `sqlite3_step` has returned `SQLITE_ROW` at least once;
    // a SCHEMA error after that point is unrecoverable (re-preparing
    // would lose the cursor position) and is surfaced to the caller.
    bool rows_seen_ = false;
  };

  // Mints a fresh memdb URI and opens a new sqlite3 handle.
  SqliteEngine();
  // Opens a new sqlite3 handle against an existing memdb URI (use
  // `filename()` of another `SqliteEngine`). The two handles share the
  // same in-memory storage via the named-MemStore feature; per-engine
  // state (functions, vtab modules, commit/rollback callbacks) is
  // fresh per engine.
  explicit SqliteEngine(const std::string& shared_filename);
  ~SqliteEngine();

  SqliteEngine(SqliteEngine&&) noexcept = delete;
  SqliteEngine& operator=(SqliteEngine&&) = delete;

  // Prepares a SQLite statement for the given SQL.
  PreparedStatement PrepareStatement(SqlSource);

  // Runs `sql` via `sqlite3_exec` with the same BUSY/LOCKED/SCHEMA retry
  // semantics as `PrepareStatement`. Intended for short literal statements
  // like SAVEPOINT/RELEASE/ROLLBACK TO that go around `PreparedStatement`.
  base::Status ExecWithRetry(const char* sql);

  // Registers a C++ function to be runnable from SQL.
  base::Status RegisterFunction(const char* name,
                                int argc,
                                Fn* fn,
                                void* ctx,
                                FnCtxDestructor* ctx_destructor,
                                bool deterministic);

  // Registers a C++ aggregate function to be runnable from SQL.
  base::Status RegisterAggregateFunction(const char* name,
                                         int argc,
                                         AggregateFnStep* step,
                                         AggregateFnFinal* final,
                                         void* ctx,
                                         FnCtxDestructor* ctx_destructor,
                                         bool deterministic);

  // Registers a C++ window function to be runnable from SQL.
  base::Status RegisterWindowFunction(const char* name,
                                      int argc,
                                      WindowFnStep* step,
                                      WindowFnInverse* inverse,
                                      WindowFnValue* value,
                                      WindowFnFinal* final,
                                      void* ctx,
                                      FnCtxDestructor* ctx_destructor,
                                      bool deterministic);

  // Unregisters a C++ function from SQL.
  base::Status UnregisterFunction(const char* name, int argc);

  // Registers a SQLite virtual table module with the given name.
  using ModuleContextDestructor = void(void*);
  void RegisterVirtualTableModule(const std::string& module_name,
                                  const sqlite3_module* module,
                                  void* ctx,
                                  ModuleContextDestructor destructor);

  // Gets the context for a registered SQL function.
  void* GetFunctionContext(const std::string& name, int argc);

  // Sets a callback to be called when a transaction is committed.
  //
  // Returns the prior context object passed to a previous invocation of this
  // function.
  //
  // See https://www.sqlite.org/c3ref/commit_hook.html for more details.
  using CommitCallback = int(void*);
  void* SetCommitCallback(CommitCallback callback, void* ctx);

  // Sets a callback to be called when a transaction is rolled back.
  //
  // Returns the prior context object passed to a previous invocation of this
  // function.
  //
  // See https://www.sqlite.org/c3ref/commit_hook.html for more details.
  using RollbackCallback = void(void*);
  void* SetRollbackCallback(RollbackCallback callback, void* ctx);

  sqlite3* db() const { return db_.get(); }
  const std::string& filename() const { return filename_; }

  // TODO: thread this through `TraceProcessor::Config` once a knob exists.
  void set_busy_retry_timeout_for_testing(base::TimeMillis timeout) {
    busy_retry_timeout_ = timeout;
  }

 private:
  using FnCtxMap =
      base::FlatHashMap<std::pair<std::string, int>, void*,
                        base::MurmurHash<std::pair<std::string, int>>>;
  std::optional<uint32_t> GetErrorOffset() const;

  std::string filename_;
  ScopedDb db_;
  // Function registrations live alongside the handle: the SQLite handle
  // owns the function pointers, but our destructor needs to drop them
  // explicitly so prepared statements get finalised before db close.
  FnCtxMap fn_ctx_;
  base::TimeMillis busy_retry_timeout_ = BusyRetryHelper::kDefaultTimeout;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_SQLITE_SQLITE_ENGINE_H_
