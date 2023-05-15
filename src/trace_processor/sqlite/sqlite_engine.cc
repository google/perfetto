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

SqliteEngine::SqliteEngine() : query_cache_(new QueryCache()) {
  sqlite3* db = nullptr;
  EnsureSqliteInitialized();
  PERFETTO_CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
  InitializeSqlite(db);
  db_.reset(std::move(db));
}

void SqliteEngine::RegisterTable(const Table& table,
                                 const std::string& table_name) {
  DbSqliteTable::Context context{query_cache_.get(),
                                 DbSqliteTable::TableComputation::kStatic,
                                 &table, nullptr};
  RegisterVirtualTableModule<DbSqliteTable>(table_name, std::move(context),
                                            SqliteTable::kEponymousOnly, false);

  // Register virtual tables into an internal 'perfetto_tables' table.
  // This is used for iterating through all the tables during a database
  // export.
  char* insert_sql = sqlite3_mprintf(
      "INSERT INTO perfetto_tables(name) VALUES('%q')", table_name.c_str());
  char* error = nullptr;
  sqlite3_exec(db_.get(), insert_sql, nullptr, nullptr, &error);
  sqlite3_free(insert_sql);
  if (error) {
    PERFETTO_ELOG("Error adding table to perfetto_tables: %s", error);
    sqlite3_free(error);
  }
}

void SqliteEngine::RegisterTableFunction(std::unique_ptr<TableFunction> fn) {
  // Figure out if the table needs explicit args (in the form of constraints
  // on hidden columns) passed to it in order to make the query valid.
  base::Status status = fn->ValidateConstraints(
      QueryConstraints(std::numeric_limits<uint64_t>::max()));

  std::string table_name = fn->TableName();
  DbSqliteTable::Context context{query_cache_.get(),
                                 DbSqliteTable::TableComputation::kDynamic,
                                 nullptr, std::move(fn)};
  RegisterVirtualTableModule<DbSqliteTable>(table_name, std::move(context),
                                            SqliteTable::kEponymousOnly, false);
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

}  // namespace trace_processor
}  // namespace perfetto
