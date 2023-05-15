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
#include <functional>
#include <memory>
#include <type_traits>

#include "perfetto/base/status.h"
#include "src/trace_processor/db/table.h"
#include "src/trace_processor/prelude/table_functions/table_function.h"
#include "src/trace_processor/sqlite/query_cache.h"
#include "src/trace_processor/sqlite/scoped_db.h"
#include "src/trace_processor/sqlite/sqlite_table.h"

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
  SqliteEngine();

  // Registers a trace processor C++ table with SQLite with an SQL name of
  // |name|.
  void RegisterTable(const Table& table, const std::string& name);

  // Registers a trace processor C++ function with SQLite.
  void RegisterTableFunction(std::unique_ptr<TableFunction> fn);

  // Registers a SQLite virtual table module with the given name.
  //
  // This API only exists for internal/legacy use: most callers should use
  // one of the RegisterTable* APIs above.
  template <typename Vtab, typename Context>
  void RegisterVirtualTableModule(const std::string& module_name,
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

  // Declares a virtual table with SQLite.
  //
  // This API only exists for internal use. Most callers should never call this
  // directly: instead use one of the RegisterTable* APIs above.
  base::Status DeclareVirtualTable(const std::string& create_stmt);

  // Saves a SQLite table across a pair of xDisconnect/xConnect callbacks.
  //
  // This API only exists for internal use. Most callers should never call this
  // directly.
  base::Status SaveSqliteTable(const std::string& table_name,
                               std::unique_ptr<SqliteTable>);

  // Restores a SQLite table across a pair of xDisconnect/xConnect callbacks.
  //
  // This API only exists for internal use. Most callers should never call this
  // directly.
  base::StatusOr<std::unique_ptr<SqliteTable>> RestoreSqliteTable(
      const std::string& table_name);

  sqlite3* db() const { return db_.get(); }

 private:
  std::unique_ptr<QueryCache> query_cache_;
  base::FlatHashMap<std::string, std::unique_ptr<SqliteTable>> saved_tables_;

  ScopedDb db_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SQLITE_SQLITE_ENGINE_H_
