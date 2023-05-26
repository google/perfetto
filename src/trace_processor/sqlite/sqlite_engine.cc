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

#include <utility>

#include "perfetto/base/status.h"
#include "src/trace_processor/sqlite/db_sqlite_table.h"
#include "src/trace_processor/sqlite/query_cache.h"
#include "src/trace_processor/sqlite/sqlite_table.h"

// In Android and Chromium tree builds, we don't have the percentile module.
// Just don't include it.
#if PERFETTO_BUILDFLAG(PERFETTO_TP_PERCENTILE)
// defined in sqlite_src/ext/misc/percentile.c
extern "C" int sqlite3_percentile_init(sqlite3* db,
                                       char** error,
                                       const sqlite3_api_routines* api);
#endif  // PERFETTO_BUILDFLAG(PERFETTO_TP_PERCENTILE)

namespace perfetto {
namespace trace_processor {
namespace {

void EnsureSqliteInitialized() {
  // sqlite3_initialize isn't actually thread-safe despite being documented
  // as such; we need to make sure multiple TraceProcessorImpl instances don't
  // call it concurrently and only gets called once per process, instead.
  static bool init_once = [] { return sqlite3_initialize() == SQLITE_OK; }();
  PERFETTO_CHECK(init_once);
}

void InitializeSqlite(sqlite3* db) {
  char* error = nullptr;
  sqlite3_exec(db, "PRAGMA temp_store=2", nullptr, nullptr, &error);
  if (error) {
    PERFETTO_FATAL("Error setting pragma temp_store: %s", error);
  }
// In Android tree builds, we don't have the percentile module.
// Just don't include it.
#if PERFETTO_BUILDFLAG(PERFETTO_TP_PERCENTILE)
  sqlite3_percentile_init(db, &error, nullptr);
  if (error) {
    PERFETTO_ELOG("Error initializing: %s", error);
    sqlite3_free(error);
  }
#endif
}

}  // namespace

SqliteEngine::SqliteEngine() {
  sqlite3* db = nullptr;
  EnsureSqliteInitialized();
  PERFETTO_CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
  InitializeSqlite(db);
  db_.reset(std::move(db));
}

SqliteEngine::~SqliteEngine() {
  // It is important to unregister any functions that have been registered with
  // the database before destroying it. This is because functions can hold onto
  // prepared statements, which must be finalized before database destruction.
  for (auto it = fn_ctx_.GetIterator(); it; ++it) {
    int ret = sqlite3_create_function_v2(db_.get(), it.key().first.c_str(),
                                         it.key().second, SQLITE_UTF8, nullptr,
                                         nullptr, nullptr, nullptr, nullptr);
    PERFETTO_CHECK(ret == 0);
  }
  fn_ctx_.Clear();
}

base::Status SqliteEngine::RegisterFunction(const char* name,
                                            int argc,
                                            Fn* fn,
                                            void* ctx,
                                            FnCtxDestructor* destructor,
                                            bool deterministic) {
  int flags = SQLITE_UTF8 | (deterministic ? SQLITE_DETERMINISTIC : 0);
  int ret =
      sqlite3_create_function_v2(db_.get(), name, static_cast<int>(argc), flags,
                                 ctx, fn, nullptr, nullptr, destructor);
  if (ret != SQLITE_OK) {
    return base::ErrStatus("Unable to register function with name %s", name);
  }
  *fn_ctx_.Insert(std::make_pair(name, argc), ctx).first = ctx;
  return base::OkStatus();
}

base::Status SqliteEngine::DeclareVirtualTable(const std::string& create_stmt) {
  int res = sqlite3_declare_vtab(db_.get(), create_stmt.c_str());
  if (res != SQLITE_OK) {
    return base::ErrStatus("Declare vtab failed: %s",
                           sqlite3_errmsg(db_.get()));
  }
  return base::OkStatus();
}

base::Status SqliteEngine::SaveSqliteTable(const std::string& table_name,
                                           std::unique_ptr<SqliteTable> table) {
  auto res = saved_tables_.Insert(table_name, {});
  if (!res.second) {
    return base::ErrStatus("Table with name %s already is saved",
                           table_name.c_str());
  }
  *res.first = std::move(table);
  return base::OkStatus();
}

base::StatusOr<std::unique_ptr<SqliteTable>> SqliteEngine::RestoreSqliteTable(
    const std::string& table_name) {
  auto* res = saved_tables_.Find(table_name);
  if (!res) {
    return base::ErrStatus("Table with name %s does not exist in saved state",
                           table_name.c_str());
  }
  return std::move(*res);
}

void* SqliteEngine::GetFunctionContext(const std::string& name, int argc) {
  auto* res = fn_ctx_.Find(std::make_pair(name, argc));
  return res ? *res : nullptr;
}

}  // namespace trace_processor
}  // namespace perfetto
