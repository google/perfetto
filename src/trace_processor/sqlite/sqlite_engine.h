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
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/hash.h"
#include "perfetto/ext/base/murmur_hash.h"
#include "src/trace_processor/sqlite/scoped_db.h"
#include "src/trace_processor/sqlite/sql_source.h"

namespace perfetto::trace_processor {

// A single open handle into a SQLite database, plus the per-handle bookkeeping
// that has to live alongside it (the function-context map). One
// |SqliteConnection| corresponds to one |sqlite3*|.
//
// In Phase 1 there is exactly one |SqliteConnection| owned by the
// |SqliteEngine|. In Phase 2 the same VFS-level in-memory database may back
// multiple |SqliteConnection|s, each with its own function registrations.
class SqliteConnection {
 public:
  using FnCtxMap = base::FlatHashMap<std::pair<std::string, int>,
                                     void*,
                                     base::MurmurHash<std::pair<std::string,
                                                                int>>>;

  // Opens a new SQLite handle against |filename|. |filename| is expected to be
  // a URI-form name pointing at the in-tree |memdb| VFS (see |SqliteEngine|).
  // Crashes (PERFETTO_CHECK) if the open fails.
  explicit SqliteConnection(const std::string& filename);
  ~SqliteConnection();

  SqliteConnection(SqliteConnection&&) noexcept = delete;
  SqliteConnection& operator=(SqliteConnection&&) = delete;
  SqliteConnection(const SqliteConnection&) = delete;
  SqliteConnection& operator=(const SqliteConnection&) = delete;

  sqlite3* db() const { return db_.get(); }
  FnCtxMap& fn_ctx() { return fn_ctx_; }
  const FnCtxMap& fn_ctx() const { return fn_ctx_; }

 private:
  // Per-handle map: function registrations are scoped to a single |sqlite3*|,
  // so this lives next to |db_|.
  FnCtxMap fn_ctx_;
  ScopedDb db_;
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

  // Wrapper class for SQLite's |sqlite3_stmt| struct and associated functions.
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

    explicit PreparedStatement(ScopedStmt, SqlSource);

    ScopedStmt stmt_;
    ScopedSqliteString expanded_sql_;
    SqlSource sql_source_;
    base::Status status_ = base::OkStatus();
  };

  // Default ctor: mints a fresh memdb URI (see |BuildMemdbUri|) and opens a
  // new SQLite handle against it. Use this for a brand-new in-memory database.
  SqliteEngine();

  // Phase 2 ctor: opens a new SQLite handle against an existing memdb URI.
  // Pass the |filename()| of an already-constructed |SqliteEngine|; the
  // resulting engine attaches to the same shared in-memory database via
  // |cache=shared|, so its connections see DDL committed on the main schema
  // by other engines pointing at the same URI.
  //
  // Other per-engine state (function/aggregate/window registrations, vtab
  // module registrations, commit/rollback callbacks) is fresh per engine —
  // sharing only happens at the storage/schema layer.
  explicit SqliteEngine(const std::string& shared_filename);

  ~SqliteEngine();

  SqliteEngine(SqliteEngine&&) noexcept = delete;
  SqliteEngine& operator=(SqliteEngine&&) = delete;

  // Prepares a SQLite statement for the given SQL.
  PreparedStatement PrepareStatement(SqlSource);

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

  sqlite3* db() const { return connection_.db(); }

  // The URI-style filename this engine's connection was opened against. In
  // Phase 2 callers can pass this to the |shared_filename| ctor of another
  // |SqliteEngine| to mint a second handle against the same shared in-memory
  // database (cross-connection schema visibility via |cache=shared|).
  const std::string& filename() const { return filename_; }

 private:
  std::optional<uint32_t> GetErrorOffset() const;

  // URI-style filename used to open all connections against this engine. Stored
  // so that future code can mint additional |SqliteConnection|s pointing at
  // the same shared in-memory database.
  std::string filename_;
  // The single connection owned by this engine today. Phase 2 will introduce
  // additional connections sharing the same |filename_|.
  SqliteConnection connection_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_SQLITE_SQLITE_ENGINE_H_
