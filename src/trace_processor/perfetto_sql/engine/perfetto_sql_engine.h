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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_PERFETTO_SQL_ENGINE_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_PERFETTO_SQL_ENGINE_H_

#include <memory>
#include <optional>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/db/runtime_table.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_parser.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_preprocessor.h"
#include "src/trace_processor/perfetto_sql/engine/runtime_table_function.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/sql_function.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/static_table_function.h"
#include "src/trace_processor/sqlite/scoped_db.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_engine.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/util/sql_modules.h"

namespace perfetto {
namespace trace_processor {

// Intermediary class which translates high-level concepts and algorithms used
// in trace processor into lower-level concepts and functions can be understood
// by and executed against SQLite.
class PerfettoSqlEngine {
 public:
  struct ExecutionStats {
    uint32_t column_count = 0;
    uint32_t statement_count = 0;
    uint32_t statement_count_with_output = 0;
  };
  struct ExecutionResult {
    SqliteEngine::PreparedStatement stmt;
    ExecutionStats stats;
  };

  explicit PerfettoSqlEngine(StringPool* pool);
  ~PerfettoSqlEngine();

  // Executes all the statements in |sql| and returns a |ExecutionResult|
  // object. The metadata will reference all the statements executed and the
  // |ScopedStmt| be empty.
  //
  // Returns an error if the execution of any statement failed or if there was
  // no valid SQL to run.
  base::StatusOr<ExecutionStats> Execute(SqlSource sql);

  // Executes all the statements in |sql| fully until the final statement and
  // returns a |ExecutionResult| object containing a |ScopedStmt| for the final
  // statement (which has been stepped once) and metadata about all statements
  // executed.
  //
  // Returns an error if the execution of any statement failed or if there was
  // no valid SQL to run.
  base::StatusOr<ExecutionResult> ExecuteUntilLastStatement(SqlSource sql);

  // Registers a trace processor C++ function to be runnable from SQL.
  //
  // The format of the function is given by the |SqlFunction|.
  //
  // |name|:        name of the function in SQL
  // |argc|:        number of arguments for this function. This can be -1 if
  //                the number of arguments is variable.
  // |ctx|:         context object for the function (see SqlFunction::Run);
  //                this object *must* outlive the function so should likely be
  //                either static or scoped to the lifetime of TraceProcessor.
  // |determistic|: whether this function has deterministic output given the
  //                same set of arguments.
  template <typename Function = SqlFunction>
  base::Status RegisterStaticFunction(const char* name,
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
  base::Status RegisterStaticFunction(
      const char* name,
      int argc,
      std::unique_ptr<typename Function::Context> ctx,
      bool deterministic = true);

  // Registers a function with the prototype |prototype| which returns a value
  // of |return_type| and is implemented by executing the SQL statement |sql|.
  base::Status RegisterRuntimeFunction(bool replace,
                                       const FunctionPrototype& prototype,
                                       std::string return_type,
                                       SqlSource sql);

  // Enables memoization for the given SQL function.
  base::Status EnableSqlFunctionMemoization(const std::string& name);

  // Registers a trace processor C++ table with SQLite with an SQL name of
  // |name|.
  void RegisterStaticTable(const Table& table, const std::string& name);

  // Registers a trace processor C++ table function with SQLite.
  void RegisterStaticTableFunction(std::unique_ptr<StaticTableFunction> fn);

  // Returns the state for the given table function.
  RuntimeTableFunction::State* GetRuntimeTableFunctionState(
      const std::string&) const;

  // Should be called when a table function is destroyed.
  void OnRuntimeTableFunctionDestroyed(const std::string&);

  SqliteEngine* sqlite_engine() { return engine_.get(); }

  // Makes new SQL module available to import.
  void RegisterModule(const std::string& name,
                      sql_modules::RegisteredModule module) {
    modules_.Insert(name, std::move(module));
  }

  // Fetches registered SQL module.
  sql_modules::RegisteredModule* FindModule(const std::string& name) {
    return modules_.Find(name);
  }

 private:
  base::StatusOr<SqlSource> ExecuteCreateFunction(
      const PerfettoSqlParser::CreateFunction&,
      const PerfettoSqlParser& parser);

  base::Status ExecuteInclude(const PerfettoSqlParser::Include&,
                              const PerfettoSqlParser& parser);

  // Registers a SQL-defined trace processor C++ table with SQLite.
  base::Status RegisterRuntimeTable(std::string name, SqlSource sql);

  base::Status ExecuteCreateView(const PerfettoSqlParser::CreateView&);

  base::Status ExecuteCreateMacro(const PerfettoSqlParser::CreateMacro&);

  std::unique_ptr<QueryCache> query_cache_;
  StringPool* pool_ = nullptr;
  base::FlatHashMap<std::string, std::unique_ptr<RuntimeTableFunction::State>>
      runtime_table_fn_states_;
  base::FlatHashMap<std::string, std::unique_ptr<RuntimeTable>> runtime_tables_;
  base::FlatHashMap<std::string, sql_modules::RegisteredModule> modules_;
  base::FlatHashMap<std::string, PerfettoSqlPreprocessor::Macro> macros_;
  std::unique_ptr<SqliteEngine> engine_;
};

}  // namespace trace_processor
}  // namespace perfetto

// The rest of this file is just implementation details which we need
// in the header file because it is templated code. We separate it out
// like this to keep the API people actually care about easy to read.

namespace perfetto {
namespace trace_processor {
namespace perfetto_sql_internal {

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

}  // namespace perfetto_sql_internal

template <typename Function>
base::Status PerfettoSqlEngine::RegisterStaticFunction(
    const char* name,
    int argc,
    typename Function::Context* ctx,
    bool deterministic) {
  return engine_->RegisterFunction(
      name, argc, perfetto_sql_internal::WrapSqlFunction<Function>, ctx,
      nullptr, deterministic);
}

template <typename Function>
base::Status PerfettoSqlEngine::RegisterStaticFunction(
    const char* name,
    int argc,
    std::unique_ptr<typename Function::Context> user_data,
    bool deterministic) {
  auto ctx_destructor = [](void* ptr) {
    delete static_cast<typename Function::Context*>(ptr);
  };
  return engine_->RegisterFunction(
      name, argc, perfetto_sql_internal::WrapSqlFunction<Function>,
      user_data.release(), ctx_destructor, deterministic);
}

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_PERFETTO_SQL_ENGINE_H_
