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
#include "src/trace_processor/prelude/functions/sql_function.h"
#include "src/trace_processor/prelude/table_functions/table_function.h"
#include "src/trace_processor/sqlite/query_cache.h"
#include "src/trace_processor/sqlite/scoped_db.h"
#include "src/trace_processor/sqlite/sqlite_table.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"

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

  // Registers a trace processor C++ function to be runnable from SQL.
  //
  // The format of the function is given by the |SqlFunction|.
  //
  // |db|:          sqlite3 database object
  // |name|:        name of the function in SQL
  // |argc|:        number of arguments for this function. This can be -1 if
  //                the number of arguments is variable.
  // |ctx|:         context object for the function (see SqlFunction::Run
  // above);
  //                this object *must* outlive the function so should likely be
  //                either static or scoped to the lifetime of TraceProcessor.
  // |determistic|: whether this function has deterministic output given the
  //                same set of arguments.
  template <typename Function = SqlFunction>
  base::Status RegisterSqlFunction(const char* name,
                                   int argc,
                                   typename Function::Context* ctx,
                                   bool deterministic = true);

  // Registers a trace processor C++ function to be runnable from SQL.
  //
  // This function is the same as the above except allows a unique_ptr to be
  // passed for the context; this allows for SQLite to manage the lifetime of
  // this pointer instead of the essentially static requirement of the context
  // pointer above.
  template <typename Function>
  base::Status RegisterSqlFunction(
      const char* name,
      int argc,
      std::unique_ptr<typename Function::Context> ctx,
      bool deterministic = true);

  // Registers a trace processor C++ table function with SQLite.
  void RegisterTableFunction(std::unique_ptr<TableFunction> fn);

  // Registers a SQLite virtual table module with the given name.
  //
  // This API only exists for internal/legacy use: most callers should use
  // one of the RegisterTable* APIs above.
  template <typename Vtab, typename Context>
  void RegisterVirtualTableModule(const std::string& module_name,
                                  Context ctx,
                                  SqliteTable::TableType table_type,
                                  bool updatable);

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

// The rest of this file is just implementation details which we need
// in the header file because it is templated code. We separate it out
// like this to keep the API people actually care about easy to read.

namespace perfetto {
namespace trace_processor {
namespace sqlite_internal {

// RAII type to call Function::Cleanup when destroyed.
template <typename Function>
struct ScopedCleanup {
  typename Function::Context* ctx;
  ~ScopedCleanup() { Function::Cleanup(ctx); }
};

template <typename Function>
void WrapSqlFunction(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
  using Context = typename Function::Context;
  Context* ud = static_cast<Context*>(sqlite3_user_data(ctx));

  ScopedCleanup<Function> scoped_cleanup{ud};
  SqlValue value{};
  SqlFunction::Destructors destructors{};
  base::Status status =
      Function::Run(ud, static_cast<size_t>(argc), argv, value, destructors);
  if (!status.ok()) {
    sqlite3_result_error(ctx, status.c_message(), -1);
    return;
  }

  if (Function::kVoidReturn) {
    if (!value.is_null()) {
      sqlite3_result_error(ctx, "void SQL function returned value", -1);
      return;
    }

    // If the function doesn't want to return anything, set the "VOID"
    // pointer type to a non-null value. Note that because of the weird
    // way |sqlite3_value_pointer| works, we need to set some value even
    // if we don't actually read it - just set it to a pointer to an empty
    // string for this reason.
    static char kVoidValue[] = "";
    sqlite3_result_pointer(ctx, kVoidValue, "VOID", nullptr);
  } else {
    sqlite_utils::ReportSqlValue(ctx, value, destructors.string_destructor,
                                 destructors.bytes_destructor);
  }

  status = Function::VerifyPostConditions(ud);
  if (!status.ok()) {
    sqlite3_result_error(ctx, status.c_message(), -1);
    return;
  }
}

}  // namespace sqlite_internal

template <typename Function>
base::Status SqliteEngine::RegisterSqlFunction(const char* name,
                                               int argc,
                                               typename Function::Context* ctx,
                                               bool deterministic) {
  int flags = SQLITE_UTF8 | (deterministic ? SQLITE_DETERMINISTIC : 0);
  int ret = sqlite3_create_function_v2(
      db_.get(), name, static_cast<int>(argc), flags, ctx,
      sqlite_internal::WrapSqlFunction<Function>, nullptr, nullptr, nullptr);
  if (ret != SQLITE_OK) {
    return base::ErrStatus("Unable to register function with name %s", name);
  }
  return base::OkStatus();
}

template <typename Function>
base::Status SqliteEngine::RegisterSqlFunction(
    const char* name,
    int argc,
    std::unique_ptr<typename Function::Context> user_data,
    bool deterministic) {
  int flags = SQLITE_UTF8 | (deterministic ? SQLITE_DETERMINISTIC : 0);
  int ret = sqlite3_create_function_v2(
      db_.get(), name, static_cast<int>(argc), flags, user_data.release(),
      sqlite_internal::WrapSqlFunction<Function>, nullptr, nullptr,
      [](void* ptr) { delete static_cast<typename Function::Context*>(ptr); });
  if (ret != SQLITE_OK) {
    return base::ErrStatus("Unable to register function with name %s", name);
  }
  return base::OkStatus();
}

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
