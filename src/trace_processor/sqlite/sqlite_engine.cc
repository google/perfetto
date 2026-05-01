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

#include "src/trace_processor/sqlite/sqlite_engine.h"

#include <sqlite3.h>
#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/sqlite/scoped_db.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/tp_metatrace.h"

#include "protos/perfetto/trace_processor/metatrace_categories.pbzero.h"

// In Android and Chromium tree builds, we don't have the percentile module.
// Just don't include it.
#if PERFETTO_BUILDFLAG(PERFETTO_TP_PERCENTILE)
// defined in sqlite_src/ext/misc/percentile.c
extern "C" int sqlite3_percentile_init(sqlite3* db,
                                       char** error,
                                       const sqlite3_api_routines* api);
#endif  // PERFETTO_BUILDFLAG(PERFETTO_TP_PERCENTILE)

namespace perfetto::trace_processor {
namespace {

void EnsureSqliteInitialized() {
  // sqlite3_initialize isn't actually thread-safe in standalone builds because
  // we build with SQLITE_THREADSAFE=0. Ensure it's only called from a single
  // thread.
  static bool init_once = [] {
    // Enabling memstatus causes a lock to be taken on every malloc/free in
    // SQLite to update the memory statistics. This can cause massive contention
    // in trace processor when multiple instances are used in parallel.
    // Fix this by disabling the memstatus API which we don't make use of in
    // any case. See b/335019324 for more info on this.
    int ret = sqlite3_config(SQLITE_CONFIG_MEMSTATUS, 0);

    // As much as it is painful, we need to catch instances of SQLITE_MISUSE
    // here against all the advice of the SQLite developers and lalitm@'s
    // intuition: SQLITE_MISUSE for sqlite3_config really means: that someone
    // else has already initialized SQLite. As we are an embeddable library,
    // it's very possible that the process embedding us has initialized SQLite
    // in a different way to what we want to do and, if so, we should respect
    // their choice.
    //
    // TODO(lalitm): ideally we would have an sqlite3_is_initialized API we
    // could use to gate the above check but that doesn't exist: report this
    // issue to SQLite developers and see if such an API could be added. If so
    // we can remove this check.
    if (ret == SQLITE_MISUSE) {
      return true;
    }

    PERFETTO_CHECK(ret == SQLITE_OK);
    return sqlite3_initialize() == SQLITE_OK;
  }();
  PERFETTO_CHECK(init_once);
}

void InitializeSqlite(sqlite3* db) {
// In Android tree builds, we don't have the percentile module.
#if PERFETTO_BUILDFLAG(PERFETTO_TP_PERCENTILE)
  char* error = nullptr;
  sqlite3_percentile_init(db, &error, nullptr);
  if (error) {
    PERFETTO_ELOG("Error initializing: %s", error);
    sqlite3_free(error);
  }
#else
  (void)db;
#endif
}

// Reads the value of |pragma| (e.g. "journal_mode") on |db| and returns it as
// a lowercase string. PERFETTO_CHECKs on any SQLite error so a silently broken
// pragma can never go unnoticed.
std::string ReadPragma(sqlite3* db, const char* pragma) {
  std::string sql = std::string("PRAGMA ") + pragma;
  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
  PERFETTO_CHECK(rc == SQLITE_OK);
  rc = sqlite3_step(stmt);
  PERFETTO_CHECK(rc == SQLITE_ROW);
  const unsigned char* text = sqlite3_column_text(stmt, 0);
  std::string out = text ? reinterpret_cast<const char*>(text) : "";
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  sqlite3_finalize(stmt);
  return out;
}

// Applies |pragma_sql| (e.g. "PRAGMA journal_mode=MEMORY"), then re-reads
// |pragma_name| and PERFETTO_CHECKs that the result matches one of
// |expected_values|. SQLite is allowed to silently fall back when a pragma
// can't be honoured (e.g. journal_mode=WAL on a backend without SHM hooks
// returns 'memory'); failing loudly here ensures future regressions surface
// at construction time rather than as subtle correctness bugs.
void ApplyAndVerifyPragma(sqlite3* db,
                          const char* pragma_sql,
                          const char* pragma_name,
                          std::initializer_list<const char*> expected_values) {
  char* error = nullptr;
  int rc = sqlite3_exec(db, pragma_sql, nullptr, nullptr, &error);
  if (rc != SQLITE_OK) {
    PERFETTO_FATAL("Error executing '%s': %s", pragma_sql,
                   error ? error : "<no message>");
  }
  std::string actual = ReadPragma(db, pragma_name);
  for (const char* expected : expected_values) {
    if (actual == expected) {
      return;
    }
  }
  PERFETTO_FATAL("Pragma '%s' silently fell back: got '%s', expected one of",
                 pragma_sql, actual.c_str());
}

std::optional<uint32_t> GetErrorOffsetDb(sqlite3* db) {
  int offset = sqlite3_error_offset(db);
  return offset == -1 ? std::nullopt
                      : std::make_optional(static_cast<uint32_t>(offset));
}

// Build a unique URI for the in-tree memdb VFS. The leading slash engages
// memdb's named-MemStore feature: multiple sqlite3 handles opened against
// the same URI share the byte buffer (the MemStore) but each gets its own
// BtShared / page cache. Sharing BtShared (via `cache=shared`) would
// serialise sqlite3_step across handles on the BtShared mutex, defeating
// multi-connection parallelism. The atomic counter prevents two
// `SqliteEngine` instances in the same process from colliding.
std::string BuildMemdbUri() {
  static std::atomic<uint64_t> kUniqueIdCounter{0};
  uint64_t unique_id = kUniqueIdCounter.fetch_add(1, std::memory_order_relaxed);
  char filename_buf[64];
  snprintf(filename_buf, sizeof(filename_buf),
           "file:/perfetto-%" PRIu64 "?vfs=memdb", unique_id);
  return filename_buf;
}

ScopedDb OpenDb(const std::string& filename) {
  EnsureSqliteInitialized();
  // SQLITE_OPEN_NOMUTEX: trace_processor uses one sqlite3 per thread, so
  // SQLite's per-handle mutex is unnecessary overhead.
  // SQLITE_OPEN_URI: URI parsing isn't enabled globally.
  static constexpr int kSqliteOpenFlags = SQLITE_OPEN_READWRITE |
                                          SQLITE_OPEN_CREATE |
                                          SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_URI;
  sqlite3* db = nullptr;
  PERFETTO_CHECK(sqlite3_open_v2(filename.c_str(), &db, kSqliteOpenFlags,
                                 nullptr) == SQLITE_OK);
  InitializeSqlite(db);

  // Multi-conn pragmas. Each is applied and verified — SQLite silently
  // falls back when a pragma can't be honoured and we want to crash
  // loudly rather than corrupt multi-conn invariants.
  // - journal_mode=MEMORY: rollback journal in memory (memdb VFS has no
  //   SHM hooks so WAL isn't an option anyway).
  // - temp_store=MEMORY: temp tables/indices in memory; substrate for
  //   the temp-then-promote include pattern. Stored as an int so the
  //   read-back is "2", not "memory".
  // - locking_mode=NORMAL: keep it from drifting to EXCLUSIVE which
  //   would break named-MemStore sharing.
  ApplyAndVerifyPragma(db, "PRAGMA journal_mode=MEMORY", "journal_mode",
                       {"memory"});
  ApplyAndVerifyPragma(db, "PRAGMA temp_store=MEMORY", "temp_store",
                       {"2", "memory"});
  ApplyAndVerifyPragma(db, "PRAGMA locking_mode=NORMAL", "locking_mode",
                       {"normal"});
  return ScopedDb(db);
}

}  // namespace

SqliteEngine::SqliteEngine()
    : filename_(BuildMemdbUri()), db_(OpenDb(filename_)) {}

SqliteEngine::SqliteEngine(const std::string& shared_filename)
    : filename_(shared_filename), db_(OpenDb(filename_)) {}

SqliteEngine::~SqliteEngine() {
  // Unregister functions before db close so any prepared statements they
  // hold get finalised first.
  for (auto it = fn_ctx_.GetIterator(); it; ++it) {
    int ret = sqlite3_create_function_v2(db_.get(), it.key().first.c_str(),
                                         it.key().second, SQLITE_UTF8, nullptr,
                                         nullptr, nullptr, nullptr, nullptr);
    if (PERFETTO_UNLIKELY(ret != SQLITE_OK)) {
      PERFETTO_FATAL("Failed to drop function: '%s'", it.key().first.c_str());
    }
  }
}

SqliteEngine::PreparedStatement SqliteEngine::PrepareStatement(SqlSource sql) {
  PERFETTO_TP_TRACE(metatrace::Category::QUERY_DETAILED, "QUERY_PREPARE");
  sqlite3* db = db_.get();
  sqlite3_stmt* raw_stmt = nullptr;
  // Transparent retry for two multi-conn signals:
  // - SQLITE_BUSY/LOCKED: a peer connection holds the MemStore file
  //   lock at SHARED/RESERVED/EXCLUSIVE. Sleep + retry.
  // - SQLITE_SCHEMA: a peer connection committed DDL since we last
  //   read page 1's schema cookie; re-prepare from source.
  // Both are safe to retry at the prepare boundary — we haven't
  // walked the b-tree yet, no rollback needed.
  int err = SQLITE_OK;
  BusyRetryHelper busy_retry(busy_retry_timeout_);
  SchemaRetryHelper schema_retry(busy_retry_timeout_);
  for (;;) {
    err = sqlite3_prepare_v2(db, sql.sql().c_str(), -1, &raw_stmt, nullptr);
    if (err == SQLITE_OK) {
      break;
    }
    if (err == SQLITE_BUSY || err == SQLITE_LOCKED) {
      if (busy_retry.ShouldRetry(err)) {
        continue;
      }
      break;
    }
    if (err == SQLITE_SCHEMA) {
      if (schema_retry.ShouldRetry(err)) {
        continue;
      }
      break;
    }
    break;
  }
  PreparedStatement statement{ScopedStmt(raw_stmt), std::move(sql), db,
                              busy_retry_timeout_};
  if (err != SQLITE_OK) {
    const char* errmsg = sqlite3_errmsg(db);
    std::string frame =
        statement.sql_source_.AsTracebackForSqliteOffset(GetErrorOffset());
    base::Status status = base::ErrStatus("%s%s", frame.c_str(), errmsg);
    status.SetPayload("perfetto.dev/has_traceback", "true");

    statement.status_ = std::move(status);
    return statement;
  }
  if (!raw_stmt) {
    statement.status_ = base::ErrStatus("No SQL to execute");
  }
  return statement;
}

base::Status SqliteEngine::ExecWithRetry(const char* sql) {
  sqlite3* db = db_.get();
  // Same retry shape as `PrepareStatement`. `sqlite3_exec` re-prepares
  // internally so a plain re-issue handles SCHEMA, and the short
  // transactional statements this is intended for (SAVEPOINT/RELEASE/
  // ROLLBACK TO) leave no partial state on BUSY/LOCKED.
  BusyRetryHelper busy_retry(busy_retry_timeout_);
  SchemaRetryHelper schema_retry(busy_retry_timeout_);
  int err = SQLITE_OK;
  char* errmsg_raw = nullptr;
  for (;;) {
    if (errmsg_raw) {
      sqlite3_free(errmsg_raw);
      errmsg_raw = nullptr;
    }
    err = sqlite3_exec(db, sql, nullptr, nullptr, &errmsg_raw);
    if (err == SQLITE_OK) {
      break;
    }
    if (err == SQLITE_BUSY || err == SQLITE_LOCKED) {
      if (busy_retry.ShouldRetry(err)) {
        continue;
      }
      break;
    }
    if (err == SQLITE_SCHEMA) {
      if (schema_retry.ShouldRetry(err)) {
        continue;
      }
      break;
    }
    break;
  }
  ScopedSqliteString errmsg(errmsg_raw);
  if (err != SQLITE_OK) {
    return base::ErrStatus("%s", errmsg_raw ? errmsg_raw : sqlite3_errmsg(db));
  }
  return base::OkStatus();
}

base::Status SqliteEngine::RegisterFunction(const char* name,
                                            int argc,
                                            Fn* fn,
                                            void* ctx,
                                            FnCtxDestructor* destructor,
                                            bool deterministic) {
  sqlite3* db = db_.get();
  int flags = SQLITE_UTF8 | (deterministic ? SQLITE_DETERMINISTIC : 0);
  int ret = sqlite3_create_function_v2(db, name, static_cast<int>(argc), flags,
                                       ctx, fn, nullptr, nullptr, destructor);
  if (ret != SQLITE_OK) {
    return base::ErrStatus(
        "Unable to register function with name %s: %s (SQLite error code: %d)",
        name, sqlite3_errmsg(db), ret);
  }
  *fn_ctx_.Insert(std::make_pair(name, argc), ctx).first = ctx;
  return base::OkStatus();
}

base::Status SqliteEngine::RegisterAggregateFunction(
    const char* name,
    int argc,
    AggregateFnStep* step,
    AggregateFnFinal* final,
    void* ctx,
    FnCtxDestructor* destructor,
    bool deterministic) {
  sqlite3* db = db_.get();
  int flags = SQLITE_UTF8 | (deterministic ? SQLITE_DETERMINISTIC : 0);
  int ret = sqlite3_create_function_v2(db, name, static_cast<int>(argc), flags,
                                       ctx, nullptr, step, final, destructor);
  if (ret != SQLITE_OK) {
    return base::ErrStatus(
        "Unable to register aggregate function with name %s: %s (SQLite error "
        "code: %d)",
        name, sqlite3_errmsg(db), ret);
  }
  return base::OkStatus();
}

base::Status SqliteEngine::RegisterWindowFunction(const char* name,
                                                  int argc,
                                                  WindowFnStep* step,
                                                  WindowFnInverse* inverse,
                                                  WindowFnValue* value,
                                                  WindowFnFinal* final,
                                                  void* ctx,
                                                  FnCtxDestructor* destructor,
                                                  bool deterministic) {
  sqlite3* db = db_.get();
  int flags = SQLITE_UTF8 | (deterministic ? SQLITE_DETERMINISTIC : 0);
  int ret = sqlite3_create_window_function(db, name, static_cast<int>(argc),
                                           flags, ctx, step, final, value,
                                           inverse, destructor);
  if (ret != SQLITE_OK) {
    return base::ErrStatus(
        "Unable to register window function with name %s: %s (SQLite error "
        "code: %d)",
        name, sqlite3_errmsg(db), ret);
  }
  return base::OkStatus();
}

base::Status SqliteEngine::UnregisterFunction(const char* name, int argc) {
  sqlite3* db = db_.get();
  int ret =
      sqlite3_create_function_v2(db, name, static_cast<int>(argc), SQLITE_UTF8,
                                 nullptr, nullptr, nullptr, nullptr, nullptr);
  if (ret != SQLITE_OK) {
    return base::ErrStatus(
        "Unable to unregister function with name %s: %s (SQLite error code: "
        "%d)",
        name, sqlite3_errmsg(db), ret);
  }
  fn_ctx_.Erase({name, argc});
  return base::OkStatus();
}

void SqliteEngine::RegisterVirtualTableModule(
    const std::string& module_name,
    const sqlite3_module* module,
    void* ctx,
    ModuleContextDestructor destructor) {
  int res = sqlite3_create_module_v2(db_.get(), module_name.c_str(), module,
                                     ctx, destructor);
  PERFETTO_CHECK(res == SQLITE_OK);
}

void* SqliteEngine::GetFunctionContext(const std::string& name, int argc) {
  auto* res = fn_ctx_.Find(std::make_pair(name, argc));
  return res ? *res : nullptr;
}

std::optional<uint32_t> SqliteEngine::GetErrorOffset() const {
  return GetErrorOffsetDb(db_.get());
}

void* SqliteEngine::SetCommitCallback(CommitCallback callback, void* ctx) {
  return sqlite3_commit_hook(db_.get(), callback, ctx);
}

void* SqliteEngine::SetRollbackCallback(RollbackCallback callback, void* ctx) {
  return sqlite3_rollback_hook(db_.get(), callback, ctx);
}

SqliteEngine::PreparedStatement::PreparedStatement(
    ScopedStmt stmt,
    SqlSource source,
    sqlite3* db,
    base::TimeMillis retry_timeout)
    : stmt_(std::move(stmt)),
      expanded_sql_(sqlite3_expanded_sql(stmt_.get())),
      sql_source_(std::move(source)),
      db_(db),
      retry_timeout_(retry_timeout) {}

int SqliteEngine::PreparedStatement::ReprepareFromSource() {
  // Finalise the stale statement before re-preparing.
  stmt_.reset();
  expanded_sql_.reset();
  sqlite3_stmt* raw_stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql_source_.sql().c_str(), -1, &raw_stmt,
                              nullptr);
  stmt_.reset(raw_stmt);
  if (rc == SQLITE_OK && raw_stmt) {
    expanded_sql_.reset(sqlite3_expanded_sql(raw_stmt));
  }
  return rc;
}

bool SqliteEngine::PreparedStatement::Step() {
  PERFETTO_TP_TRACE(metatrace::Category::QUERY_DETAILED, "STMT_STEP",
                    [this](metatrace::Record* record) {
                      record->AddArg("Original SQL", original_sql());
                      record->AddArg("Executed SQL", sql());
                    });

  // Transparent retry for two multi-conn signals:
  // - SQLITE_BUSY/LOCKED: a peer connection holds the MemStore file
  //   lock. Reset the statement and retry.
  // - SQLITE_SCHEMA: a peer connection bumped the schema cookie; the
  //   bytecode is stale. Re-prepare from `sql_source_` and retry. If
  //   a row has already been yielded, the cursor can't be safely
  //   restarted — surface the error instead.
  int err = SQLITE_OK;
  BusyRetryHelper busy_retry(retry_timeout_);
  SchemaRetryHelper schema_retry(retry_timeout_);
  for (;;) {
    err = sqlite3_step(stmt_.get());
    if (err == SQLITE_BUSY || err == SQLITE_LOCKED) {
      sqlite3_reset(stmt_.get());
      if (busy_retry.ShouldRetry(err)) {
        continue;
      }
      break;
    }
    if (err == SQLITE_SCHEMA) {
      if (rows_seen_) {
        break;  // can't restart mid-cursor; surface the error.
      }
      if (!schema_retry.ShouldRetry(err)) {
        break;
      }
      // Re-prepare can itself hit SCHEMA or BUSY/LOCKED; both are
      // absorbed by the same retry budgets here.
      int rc = ReprepareFromSource();
      while (rc != SQLITE_OK) {
        if (rc == SQLITE_SCHEMA && schema_retry.ShouldRetry(rc)) {
          rc = ReprepareFromSource();
          continue;
        }
        if ((rc == SQLITE_BUSY || rc == SQLITE_LOCKED) &&
            busy_retry.ShouldRetry(rc)) {
          rc = ReprepareFromSource();
          continue;
        }
        break;
      }
      if (rc != SQLITE_OK) {
        err = rc;
        break;
      }
      continue;
    }
    break;
  }
  if (err == SQLITE_ROW) {
    rows_seen_ = true;
    return true;
  }
  if (err == SQLITE_DONE) {
    return false;
  }
  sqlite3* db = db_;
  std::string frame =
      sql_source_.AsTracebackForSqliteOffset(GetErrorOffsetDb(db));
  const char* errmsg = sqlite3_errmsg(db);
  status_ = base::ErrStatus("%s%s", frame.c_str(), errmsg);
  return false;
}

// =====================
// BusyRetryHelper impl.
// =====================

BusyRetryHelper::BusyRetryHelper(base::TimeMillis timeout)
    : deadline_(base::GetWallTimeMs() + timeout),
      sleep_fn_(&base::SleepMicroseconds) {}

bool BusyRetryHelper::ShouldRetry(int sqlite_status) {
  if (sqlite_status != SQLITE_BUSY && sqlite_status != SQLITE_LOCKED) {
    return false;
  }
  if (base::GetWallTimeMs() >= deadline_) {
    return false;
  }
  // Capped exponential backoff: 100us, 1ms, 10ms, then 50ms.
  static constexpr unsigned kBackoffSchedule[] = {100, 1000, 10000, 50000};
  unsigned interval_us = kBackoffSchedule[std::min<size_t>(
      attempt_, base::ArraySize(kBackoffSchedule) - 1)];
  attempt_++;
  sleep_fn_(interval_us);
  return true;
}

// =====================
// SchemaRetryHelper impl.
// =====================

SchemaRetryHelper::SchemaRetryHelper(base::TimeMillis timeout)
    : deadline_(base::GetWallTimeMs() + timeout) {}

bool SchemaRetryHelper::ShouldRetry(int sqlite_status) {
  if (sqlite_status != SQLITE_SCHEMA) {
    return false;
  }
  if (attempt_ >= kMaxAttempts) {
    return false;
  }
  if (base::GetWallTimeMs() >= deadline_) {
    return false;
  }
  attempt_++;
  return true;
}

bool SqliteEngine::PreparedStatement::IsDone() const {
  return !sqlite3_stmt_busy(stmt_.get());
}

const char* SqliteEngine::PreparedStatement::original_sql() const {
  return sql_source_.original_sql().c_str();
}

const char* SqliteEngine::PreparedStatement::sql() const {
  return expanded_sql_.get();
}

}  // namespace perfetto::trace_processor
