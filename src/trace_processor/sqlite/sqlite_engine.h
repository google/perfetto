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
#include <stdint.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/hash.h"
#include "src/trace_processor/db/table.h"
#include "src/trace_processor/sqlite/query_cache.h"
#include "src/trace_processor/sqlite/scoped_db.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_table.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/tp_metatrace.h"

namespace perfetto {
namespace trace_processor {

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

  SqliteEngine();
  ~SqliteEngine();

  // Prepares a SQLite statement for the given SQL.
  PreparedStatement PrepareStatement(SqlSource);

  // Registers a C++ function to be runnable from SQL.
  base::Status RegisterFunction(const char* name,
                                int argc,
                                Fn* fn,
                                void* ctx,
                                FnCtxDestructor* ctx_destructor,
                                bool deterministic);

  // Registers a SQLite virtual table module with the given name.
  template <typename Vtab, typename Context>
  void RegisterVirtualTableModule(const std::string& module_name,
                                  Context ctx,
                                  SqliteTable::TableType table_type,
                                  bool updatable);

  // Declares a virtual table with SQLite.
  base::Status DeclareVirtualTable(const std::string& create_stmt);

  // Saves a SQLite table across a pair of xDisconnect/xConnect callbacks.
  base::Status SaveSqliteTable(const std::string& table_name,
                               std::unique_ptr<SqliteTable>);

  // Restores a SQLite table across a pair of xDisconnect/xConnect callbacks.
  base::StatusOr<std::unique_ptr<SqliteTable>> RestoreSqliteTable(
      const std::string& table_name);

  // Gets the context for a registered SQL function.
  void* GetFunctionContext(const std::string& name, int argc);

  // Should be called when a SqliteTable instance is created.
  void OnSqliteTableCreated(const std::string& name, SqliteTable::TableType);

  // Should be called when a SqliteTable instance is destroyed.
  void OnSqliteTableDestroyed(const std::string& name);

  sqlite3* db() const { return db_.get(); }

 private:
  struct FnHasher {
    size_t operator()(const std::pair<std::string, int>& x) const {
      base::Hasher hasher;
      hasher.Update(x.first);
      hasher.Update(x.second);
      return static_cast<size_t>(hasher.digest());
    }
  };

  std::optional<uint32_t> GetErrorOffset() const;

  SqliteEngine(SqliteEngine&&) noexcept = delete;
  SqliteEngine& operator=(SqliteEngine&&) = delete;

  base::FlatHashMap<std::string, SqliteTable::TableType> sqlite_tables_;
  base::FlatHashMap<std::string, std::unique_ptr<SqliteTable>> saved_tables_;
  base::FlatHashMap<std::pair<std::string, int>, void*, FnHasher> fn_ctx_;

  ScopedDb db_;
};

}  // namespace trace_processor
}  // namespace perfetto

// The rest of this file is just implementation details which we need
// in the header file because it is templated code. We separate it out
// like this to keep the API people actually care about easy to read.

namespace perfetto {
namespace trace_processor {

template <typename Vtab, typename Context>
void SqliteEngine::RegisterVirtualTableModule(const std::string& module_name,
                                              Context ctx,
                                              SqliteTable::TableType table_type,
                                              bool updatable) {
  static_assert(std::is_base_of_v<SqliteTable, Vtab>,
                "Must subclass TypedSqliteTable");

  auto module_arg =
      Vtab::CreateModuleArg(this, std::move(ctx), table_type, updatable);
  sqlite3_module* module = &module_arg->module;
  int res = sqlite3_create_module_v2(
      db_.get(), module_name.c_str(), module, module_arg.release(),
      [](void* arg) { delete static_cast<typename Vtab::ModuleArg*>(arg); });
  PERFETTO_CHECK(res == SQLITE_OK);
}

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SQLITE_SQLITE_ENGINE_H_
