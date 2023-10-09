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

#include <memory>
#include <optional>
#include <utility>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/sqlite/db_sqlite_table.h"
#include "src/trace_processor/sqlite/query_cache.h"
#include "src/trace_processor/sqlite/scoped_db.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_table.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"

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

std::optional<uint32_t> GetErrorOffsetDb(sqlite3* db) {
  int offset = sqlite3_error_offset(db);
  return offset == -1 ? std::nullopt
                      : std::make_optional(static_cast<uint32_t>(offset));
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
  // IMPORTANT: the order of operations in this destructor is very sensitive and
  // should not be changed without careful consideration of the consequences.
  // Thankfully, because we are very aggressive with PERFETTO_CHECK, mistakes
  // will usually manifest as crashes, but this is not guaranteed.

  // Drop any explicitly created virtual tables before destroying the database
  // so that any prepared statements are correctly finalized. Note that we need
  // to do this in two steps (first create all the SQLs before then executing
  // them) because |OnSqliteTableDestroyed| will be called as each DROP is
  // executed.
  std::vector<std::string> drop_stmts;
  for (auto it = sqlite_tables_.GetIterator(); it; ++it) {
    if (it.value() != SqliteTable::TableType::kExplicitCreate) {
      continue;
    }
    base::StackString<1024> drop("DROP TABLE %s", it.key().c_str());
    drop_stmts.emplace_back(drop.ToStdString());
  }
  for (const auto& drop : drop_stmts) {
    int ret = sqlite3_exec(db(), drop.c_str(), nullptr, nullptr, nullptr);
    PERFETTO_CHECK(ret == SQLITE_OK);
  }

  // It is important to unregister any functions that have been registered with
  // the database before destroying it. This is because functions can hold onto
  // prepared statements, which must be finalized before database destruction.
  for (auto it = fn_ctx_.GetIterator(); it; ++it) {
    int ret = sqlite3_create_function_v2(db_.get(), it.key().first.c_str(),
                                         it.key().second, SQLITE_UTF8, nullptr,
                                         nullptr, nullptr, nullptr, nullptr);
    PERFETTO_CHECK(ret == SQLITE_OK);
  }
  fn_ctx_.Clear();

  // Reset the database itself.
  db_.reset();

  // SQLite is not guaranteed to pick saved tables back up when destroyed as
  // from it's perspective, it has called xDisconnect. Make sure to do that
  // ourselves.
  saved_tables_.Clear();

  // The above operations should have cleared all the tables.
  PERFETTO_CHECK(sqlite_tables_.size() == 0);
}

SqliteEngine::PreparedStatement SqliteEngine::PrepareStatement(SqlSource sql) {
  PERFETTO_TP_TRACE(metatrace::Category::QUERY_DETAILED, "QUERY_PREPARE");
  sqlite3_stmt* raw_stmt = nullptr;
  int err =
      sqlite3_prepare_v2(db_.get(), sql.sql().c_str(), -1, &raw_stmt, nullptr);
  PreparedStatement statement{ScopedStmt(raw_stmt), std::move(sql)};
  if (err != SQLITE_OK) {
    const char* errmsg = sqlite3_errmsg(db_.get());
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
  std::unique_ptr<SqliteTable> table = std::move(*res);
  PERFETTO_CHECK(saved_tables_.Erase(table_name));
  return std::move(table);
}

void* SqliteEngine::GetFunctionContext(const std::string& name, int argc) {
  auto* res = fn_ctx_.Find(std::make_pair(name, argc));
  return res ? *res : nullptr;
}

std::optional<uint32_t> SqliteEngine::GetErrorOffset() const {
  return GetErrorOffsetDb(db_.get());
}

void SqliteEngine::OnSqliteTableCreated(const std::string& name,
                                        SqliteTable::TableType type) {
  auto it_and_inserted = sqlite_tables_.Insert(name, type);
  PERFETTO_CHECK(it_and_inserted.second);
}

void SqliteEngine::OnSqliteTableDestroyed(const std::string& name) {
  PERFETTO_CHECK(sqlite_tables_.Erase(name));
}

SqliteEngine::PreparedStatement::PreparedStatement(ScopedStmt stmt,
                                                   SqlSource source)
    : stmt_(std::move(stmt)),
      expanded_sql_(sqlite3_expanded_sql(stmt_.get())),
      sql_source_(std::move(source)) {}

bool SqliteEngine::PreparedStatement::Step() {
  PERFETTO_TP_TRACE(metatrace::Category::QUERY_DETAILED, "STMT_STEP",
                    [this](metatrace::Record* record) {
                      record->AddArg("Original SQL", original_sql());
                      record->AddArg("Executed SQL", sql());
                    });

  // Now step once into |cur_stmt| so that when we prepare the next statment
  // we will have executed any dependent bytecode in this one.
  int err = sqlite3_step(stmt_.get());
  if (err == SQLITE_ROW) {
    return true;
  }
  if (err == SQLITE_DONE) {
    return false;
  }
  sqlite3* db = sqlite3_db_handle(stmt_.get());
  std::string frame =
      sql_source_.AsTracebackForSqliteOffset(GetErrorOffsetDb(db));
  const char* errmsg = sqlite3_errmsg(db);
  status_ = base::ErrStatus("%s%s", frame.c_str(), errmsg);
  return false;
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

}  // namespace trace_processor
}  // namespace perfetto
