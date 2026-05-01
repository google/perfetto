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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_PERFETTO_SQL_CONNECTION_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_PERFETTO_SQL_CONNECTION_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/perfetto_sql/engine/dataframe_module.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_database.h"
#include "src/trace_processor/perfetto_sql/engine/runtime_table_function.h"
#include "src/trace_processor/perfetto_sql/engine/static_table_function_module.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/static_table_function.h"
#include "src/trace_processor/perfetto_sql/parser/function_util.h"
#include "src/trace_processor/perfetto_sql/parser/perfetto_sql_parser.h"
#include "src/trace_processor/sqlite/bindings/sqlite_module.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_window_function.h"
#include "src/trace_processor/sqlite/module_state_manager.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_engine.h"
#include "src/trace_processor/util/sql_argument.h"
#include "src/trace_processor/util/sql_modules.h"

namespace perfetto::trace_processor {

// A single connection to the trace processor SQL engine: owns one
// `sqlite3*` handle and the per-connection state layered on top of
// it (PerfettoSQL parser stack, intrinsic function and macro
// registries, virtual-table-module state managers, execution stack
// for nested INCLUDE/wildcard handling).
//
// Cross-connection state — vtab-state map, function pool, package
// map, per-module include-claim machinery — lives on the
// `PerfettoSqlDatabase` passed in at construction. Connections diff
// against the database's additive function pool at the top of every
// `Execute` so dynamic functions registered on the writer become
// visible on readers.
//
// Thread-affinity: a connection (and the iterators / prepared
// statements it produces) is owned by exactly one thread at a time.
// Multiple connections — each on its own thread — may execute
// concurrently against the same `PerfettoSqlDatabase`.
class PerfettoSqlConnection {
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
  struct StaticTable {
    dataframe::Dataframe* dataframe;
    std::string name;
  };
  // Primary (writer) ctor: opens a fresh sqlite3 handle and registers
  // the full set of tables, functions, and vtab modules.
  PerfettoSqlConnection(StringPool* pool,
                        bool enable_extra_checks,
                        PerfettoSqlDatabase* database);

  // Secondary (reader) ctor: opens a sqlite3 handle against
  // `shared_filename` (use `SqliteEngine::filename()` of the primary).
  // The two handles share the in-memory storage via the named-MemStore
  // feature, so plain SQL / DDL committed on `main` by the primary is
  // visible here. Per-connection state (function/aggregate/window
  // registrations, vtab modules, commit/rollback callbacks) is fresh.
  //
  // Intentional limitations: only the dataframe vtab module is
  // registered on secondaries; runtime-table-function / static-table-
  // function vtabs are not yet replicated across connections.
  // PerfettoSQL scalar functions flow in via the database's additive
  // function pool diff at the top of every `Execute`; package
  // registrations are looked up directly on the database.
  PerfettoSqlConnection(StringPool* pool,
                        bool enable_extra_checks,
                        const std::string& shared_filename,
                        PerfettoSqlDatabase* database);

  // Initializes the static tables and functions in the engine.
  base::Status InitializeStaticTablesAndFunctions(
      const std::vector<StaticTable>& tables,
      std::vector<std::unique_ptr<StaticTableFunction>> functions);

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

  // Prepares a single SQLite statement in |sql| and returns a
  // |PreparedStatement| object.
  //
  // Returns an error if the preparation of the statement failed or if there was
  // no valid SQL to run.
  base::StatusOr<SqliteEngine::PreparedStatement> PrepareSqliteStatement(
      SqlSource sql);

  // Registers a virtual table module with the given name.
  //
  // |name|: name of the module in SQL.
  // |ctx|:  context object for the module. This object *must* outlive the
  //         module so should likely be either static or scoped to the lifetime
  //         of TraceProcessor.
  template <typename Module>
  void RegisterVirtualTableModule(const char* name,
                                  typename Module::Context* ctx) {
    static_assert(std::is_base_of_v<sqlite::Module<Module>, Module>,
                  "Must subclass sqlite::Module");
    // If the context class of the module inherits from
    // ModuleStateManagerBase, we need to add it to the list of virtual module
    // state managers so it receives the OnCommit/OnRollback callbacks.
    if constexpr (std::is_base_of_v<sqlite::ModuleStateManagerBase,
                                    typename Module::Context>) {
      virtual_module_state_managers_.push_back(ctx);
    }
    engine_->RegisterVirtualTableModule(name, &Module::kModule, ctx, nullptr);
  }

  // Registers a virtual table module with the given name.
  //
  // |name|: name of the module in SQL.
  // |ctx|:  context object for the module. The lifetime of the context object
  //         is managed by SQLite.
  template <typename Module>
  void RegisterVirtualTableModule(
      const char* name,
      std::unique_ptr<typename Module::Context> ctx) {
    static_assert(std::is_base_of_v<sqlite::Module<Module>, Module>,
                  "Must subclass sqlite::Module");
    // If the context class of the module inherits from
    // ModuleStateManagerBase, we need to add it to the list of virtual module
    // state managers so it receives the OnCommit/OnRollback callbacks.
    if constexpr (std::is_base_of_v<sqlite::ModuleStateManagerBase,
                                    typename Module::Context>) {
      virtual_module_state_managers_.push_back(ctx.get());
    }
    engine_->RegisterVirtualTableModule(
        name, &Module::kModule, ctx.release(),
        [](void* ptr) { delete static_cast<typename Module::Context*>(ptr); });
  }

  // Registers a virtual table module from a plugin's SqliteModuleRegistration.
  void RegisterSqliteModuleForPlugin(const char* name,
                                     const sqlite3_module* module,
                                     void* ctx,
                                     void (*destructor)(void*),
                                     bool is_state_manager) {
    if (is_state_manager) {
      virtual_module_state_managers_.push_back(
          static_cast<sqlite::ModuleStateManagerBase*>(ctx));
    }
    engine_->RegisterVirtualTableModule(name, module, ctx, destructor);
  }

  // Registers a trace processor C++ function to be runnable from SQL.
  //
  // Uses the direct SQLite function interface. This is the preferred method
  // for registering new functions.
  //
  // The format of the function is given by the |sqlite::Function|.
  //
  // |ctx|:           context object for the function; this object *must*
  //                  outlive the function so should likely be either static or
  //                  scoped to the lifetime of TraceProcessor.
  // |deterministic|: whether this function has deterministic output given the
  //                  same set of arguments.
  // Arguments for RegisterFunction with custom function names.
  struct RegisterFunctionArgs {
    RegisterFunctionArgs(const char* _name = nullptr,
                         bool _deterministic = true,
                         std::optional<int> _argc = std::nullopt)
        : name(_name), deterministic(_deterministic), argc(_argc) {}
    const char* name = nullptr;  // If nullptr, uses Function::kName
    bool deterministic = true;
    std::optional<int> argc =
        std::nullopt;  // If nullopt, uses Function::kArgCount
  };

  template <typename Function>
  base::Status RegisterFunction(typename Function::UserData* ctx,
                                const RegisterFunctionArgs& args = {});
  template <typename Function>
  base::Status RegisterFunction(
      std::unique_ptr<typename Function::UserData> ctx,
      const RegisterFunctionArgs& args = {});

  // Registers a trace processor C++ aggregate function to be runnable from SQL.
  //
  // Uses the direct SQLite aggregate function interface. This is the preferred
  // method for registering new aggregate functions.
  //
  // The format of the function is given by the |SqliteAggregateFunction|.
  //
  // |ctx|:           context object for the function; this object *must*
  //                  outlive the function so should likely be either static or
  //                  scoped to the lifetime of TraceProcessor.
  // |deterministic|: whether this function has deterministic output given the
  //                  same set of arguments.
  template <typename Function>
  base::Status RegisterAggregateFunction(typename Function::UserData* ctx,
                                         bool deterministic = true);

  // Registers a trace processor C++ window function to be runnable from SQL.
  //
  // Uses the direct SQLite window function interface. This is the preferred
  // method for registering new window functions.
  //
  // The format of the function is given by the |SqliteWindowFunction|.
  //
  // |name|:          name of the function in SQL.
  // |argc|:          number of arguments for this function. This can be -1 if
  //                  the number of arguments is variable.
  // |ctx|:           context object for the function; this object *must*
  //                  outlive the function so should likely be either static or
  //                  scoped to the lifetime of TraceProcessor.
  // |deterministic|: whether this function has deterministic output given the
  //                  same set of arguments.
  template <typename Function = sqlite::WindowFunction>
  base::Status RegisterWindowFunction(const char* name,
                                      int argc,
                                      typename Function::Context* ctx,
                                      bool deterministic = true);

  // Enables memoization for the given SQL function.
  base::Status EnableSqlFunctionMemoization(const std::string& name);

  SqliteEngine* sqlite_engine() { return engine_.get(); }

  // Canonical accessor for the underlying sqlite3 handle.
  sqlite3* db() { return engine_->db(); }

  // Returns the number of objects (tables, views, functions etc) registered
  // with SQLite.
  uint64_t SqliteRegisteredObjectCount() {
    // This query will return all the tables, views, indexes and table functions
    // SQLite knows about.
    constexpr char kAllTablesQuery[] =
        "SELECT COUNT() FROM (SELECT * FROM sqlite_master "
        "UNION ALL SELECT * FROM sqlite_temp_master)";
    auto stmt = ExecuteUntilLastStatement(
        SqlSource::FromTraceProcessorImplementation(kAllTablesQuery));
    if (!stmt.ok()) {
      PERFETTO_FATAL("%s", stmt.status().c_message());
    }
    uint32_t query_count =
        static_cast<uint32_t>(sqlite3_column_int(stmt->stmt.sqlite_stmt(), 0));
    PERFETTO_CHECK(!stmt->stmt.Step());
    PERFETTO_CHECK(stmt->stmt.status().ok());

    // The missing objects from the above query are functions and macros.
    // Add those in now.
    return query_count + function_count_ + window_function_count_ +
           aggregate_function_count_ + macros_.size();
  }

  // Find dataframe registered with engine with provided name.
  const dataframe::Dataframe* GetDataframeOrNull(const std::string& name) const;

  // Registers a function with the prototype |prototype| which returns a value
  // of |return_type| and is implemented by executing the SQL statement |sql|.
  //
  // LEGACY: This function uses SQL-based function definitions. For new code,
  // prefer RegisterFunction() which uses C++ implementations.
  base::Status RegisterLegacyRuntimeFunction(bool replace,
                                             const FunctionPrototype& prototype,
                                             sql_argument::Type return_type,
                                             SqlSource sql);

 private:
  enum class FrameType { kRoot, kInclude, kWildcard };

  // Result of processing a single frame iteration.
  enum class FrameResult {
    kContinue,     // Frame still has work, continue processing it
    kFrameDone,    // Frame completed, should be popped
    kReturnResult  // Root frame completed with result
  };

  // Represents the execution state for a single SQL source being executed.
  struct ExecutionFrame {
    FrameType type = FrameType::kRoot;

    // For root and include frames: the SQL being executed
    SqlSource sql_source;
    std::unique_ptr<PerfettoSqlParser> parser;
    ExecutionStats accumulated_stats;
    std::optional<SqliteEngine::PreparedStatement> current_stmt;

    // For include frames: metadata needed to complete the include
    std::string include_key;
    SqlSource traceback_sql;

    // For wildcard frames: (key, sql) pairs to include in order.
    std::vector<std::pair<std::string, std::string>> wildcard_modules;
    size_t wildcard_index = 0;
    SqlSource wildcard_traceback_sql;

    // For include frames: the savepoint name created when the frame
    // was pushed (empty if open failed). RELEASEd on success so the
    // DDL becomes visible on `main`; rolled back on failure.
    std::string include_savepoint;
    // For include frames: the cross-connection include claim acquired
    // before the savepoint was opened. Released on successful RELEASE;
    // destructor is the rollback path (treated as failure).
    PerfettoSqlDatabase::IncludeClaim include_claim;
  };

  void RegisterStaticTable(dataframe::Dataframe*, const std::string&);
  void RegisterStaticTableFunction(std::unique_ptr<StaticTableFunction> fn);

  base::Status ExecuteCreateFunction(const PerfettoSqlParser::CreateFunction&);

  base::Status RegisterDelegatingFunction(
      const PerfettoSqlParser::CreateFunction&);

  base::Status RegisterFunctionAndAddToRegistry(
      const char* name,
      int argc,
      SqliteEngine::Fn* func,
      void* ctx,
      SqliteEngine::FnCtxDestructor* ctx_destructor,
      bool deterministic);

  base::Status ExecuteInclude(const PerfettoSqlParser::Include&,
                              const PerfettoSqlParser& parser);

  // Creates a runtime table and registers it with SQLite.
  base::Status ExecuteCreateTable(
      const PerfettoSqlParser::CreateTable& create_table);

  base::Status ExecuteCreateView(const PerfettoSqlParser::CreateView&);

  base::Status ExecuteCreateMacro(const PerfettoSqlParser::CreateMacro&);

  base::Status ExecuteCreateIndex(const PerfettoSqlParser::CreateIndex&);

  base::Status DropIndexBeforeCreate(const PerfettoSqlParser::CreateIndex&);

  base::Status ExecuteDropIndex(const PerfettoSqlParser::DropIndex&);

  base::Status ExecuteCreateTableUsingRuntimeTable(
      const PerfettoSqlParser::CreateTable& create_table,
      SqliteEngine::PreparedStatement stmt,
      const std::vector<std::string>& column_names,
      const std::vector<sql_argument::ArgumentDefinition>& effective_schema);

  enum class CreateTableType {
    kCreateTable,
    // For now, bytes columns are not supported in CREATE PERFETTO TABLE,
    // but supported in CREATE PERFETTO VIEW, so we skip them when validating
    // views.
    kValidateOnly
  };

  // Given a package and a module key, includes the matching file(s).
  // The key may contain a wildcard.
  base::Status IncludePackageImpl(sql_modules::RegisteredPackage&,
                                  const std::string& key,
                                  const PerfettoSqlParser&);
  base::Status IncludeModuleImpl(const std::string& sql,
                                 const std::string& key,
                                 const PerfettoSqlParser&);

  // Implementation of ExecuteUntilLastStatement. Separated to handle
  // re-entrant Execute() calls from statement handlers.
  base::StatusOr<ExecutionResult> ExecuteUntilLastStatementImpl(SqlSource);

  // Local-only function registration: used by both the public
  // `RegisterLegacyRuntimeFunction` (writer; then publishes to the
  // pool) and `SyncFunctionsFromPool` (reader; consumes the pool).
  base::Status RegisterLegacyRuntimeFunctionLocal(
      bool replace,
      const FunctionPrototype& prototype,
      sql_argument::Type return_type,
      SqlSource sql);

  // Pool diff at the top of every top-level `Execute`. Cheap fast-
  // path when `last_synced_function_version_` already matches the
  // pool's latest. Writer engines short-circuit — they're the source
  // of truth and bump the version when they Append.
  base::Status SyncFunctionsFromPool();

  // One iteration of the frame at `frame_idx`. May push new frames
  // onto the stack (for includes / wildcards).
  base::StatusOr<FrameResult> ProcessFrame(size_t frame_idx);

  // Per-include savepoint open/release/rollback. Rollback is best-
  // effort: failures are logged because the caller already has the
  // primary error to report.
  base::Status ReleaseIncludeSavepoint(const ExecutionFrame& frame);
  void RollbackIncludeSavepoint(const ExecutionFrame& frame);

  // Top-level `Execute` is wrapped in a uniquely-named SAVEPOINT
  // (`perfetto_execute_<n>`) so multi-statement SQL is atomic: if a
  // later statement fails, earlier side-effects are rolled back.
  base::StatusOr<std::string> OpenExecuteSavepoint();
  base::Status ReleaseExecuteSavepoint(const std::string& name);
  void RollbackExecuteSavepoint(const std::string& name);

  // SQLite transaction hooks. See
  // https://www.sqlite.org/lang_transaction.html.
  int OnCommit();
  void OnRollback();

  // ===== Cross-connection (database) state =====
  // The only field below pointing at shared state. Owned by the
  // parent `TraceProcessorImpl`; outlives this connection.
  PerfettoSqlDatabase* database_ = nullptr;

  // ===== Per-connection state =====
  StringPool* pool_ = nullptr;

  // If true, perform additional consistency checks when e.g. creating
  // tables and views.
  const bool enable_extra_checks_;

  // Function-pool version last synced from `database_->functions`.
  // Bumped on `Append` (so we don't re-register our own entries) and
  // on `SyncFunctionsFromPool` (after registering a peer's entries).
  uint64_t last_synced_function_version_ = 0;

  // Execution stack for iterative (non-recursive) processing of SQL
  // sources. When an INCLUDE statement is encountered, the included
  // module's SQL is pushed onto this stack and executed before
  // continuing with the current SQL.
  std::vector<ExecutionFrame> execution_stack_;

  uint64_t function_count_ = 0;
  uint64_t aggregate_function_count_ = 0;
  uint64_t window_function_count_ = 0;

  // Monotonically increasing counter used to generate unique SAVEPOINT names
  // for the temp-then-promote include pattern.
  uint64_t include_savepoint_counter_ = 0;

  // Monotonically increasing counter used to generate unique SAVEPOINT names
  // wrapping every top-level `ExecuteUntilLastStatement` invocation for
  // multi-statement atomicity.
  uint64_t execute_savepoint_counter_ = 0;

  // Contains the pointers for all registered virtual table modules where the
  // context class of the module inherits from ModuleStateManagerBase.
  std::vector<sqlite::ModuleStateManagerBase*> virtual_module_state_managers_;

  RuntimeTableFunctionModule::Context* runtime_table_fn_context_ = nullptr;
  StaticTableFunctionModule::Context* static_table_fn_context_ = nullptr;
  DataframeModule::Context* dataframe_context_ = nullptr;
  base::FlatHashMap<std::string, PerfettoSqlParser::Macro> macros_;

  // Registry of intrinsic functions that can be aliased
  // Maps intrinsic_name -> (function_ptr, argc, ctx, deterministic)
  struct IntrinsicFunctionInfo {
    SqliteEngine::Fn* func;
    int argc;
    void* ctx;
    bool deterministic;
  };
  base::FlatHashMap<std::string, IntrinsicFunctionInfo>
      intrinsic_function_registry_;

  std::unique_ptr<SqliteEngine> engine_;
};

// The rest of this file is just implementation details which we need
// in the header file because it is templated code. We separate it out
// like this to keep the API people actually care about easy to read.

template <typename Function>
base::Status PerfettoSqlConnection::RegisterFunction(
    typename Function::UserData* ctx,
    const RegisterFunctionArgs& args) {
  function_count_++;
  const char* name = args.name ? args.name : Function::kName;
  int argc = args.argc.has_value() ? args.argc.value() : Function::kArgCount;
  return RegisterFunctionAndAddToRegistry(name, argc, Function::Step, ctx,
                                          nullptr, args.deterministic);
}

template <typename Function>
base::Status PerfettoSqlConnection::RegisterFunction(
    std::unique_ptr<typename Function::UserData> ctx,
    const RegisterFunctionArgs& args) {
  function_count_++;
  const char* name = args.name ? args.name : Function::kName;
  int argc = args.argc.has_value() ? args.argc.value() : Function::kArgCount;
  return RegisterFunctionAndAddToRegistry(
      name, argc, Function::Step, ctx.release(),
      [](void* ptr) {
        std::unique_ptr<typename Function::UserData>(
            static_cast<typename Function::UserData*>(ptr));
      },
      args.deterministic);
}

template <typename Function>
base::Status PerfettoSqlConnection::RegisterAggregateFunction(
    typename Function::UserData* ctx,
    bool deterministic) {
  aggregate_function_count_++;
  return engine_->RegisterAggregateFunction(
      Function::kName, Function::kArgCount, Function::Step, Function::Final,
      ctx, nullptr, deterministic);
}

template <typename Function>
base::Status PerfettoSqlConnection::RegisterWindowFunction(
    const char* name,
    int argc,
    typename Function::Context* ctx,
    bool deterministic) {
  window_function_count_++;
  return engine_->RegisterWindowFunction(
      name, argc, Function::Step, Function::Inverse, Function::Value,
      Function::Final, ctx, nullptr, deterministic);
}

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_PERFETTO_SQL_CONNECTION_H_
