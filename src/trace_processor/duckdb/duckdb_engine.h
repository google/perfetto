/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_DUCKDB_DUCKDB_ENGINE_H_
#define SRC_TRACE_PROCESSOR_DUCKDB_DUCKDB_ENGINE_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "duckdb.h"

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/duckdb/arg_set_json_function.h"
#include "src/trace_processor/duckdb/duckdb_iterator_impl.h"
#include "src/trace_processor/duckdb/extract_arg.h"
#include "src/trace_processor/duckdb/table_provider.h"

namespace perfetto::trace_processor {
class StringPool;
class ClockConverter;
}  // namespace perfetto::trace_processor

namespace perfetto::trace_processor::duckdb_integration {

// The engine-facing entry point for the experimental DuckDB query engine (the
// SQLite -> DuckDB migration). Owns a lazily-created DuckDB database/connection
// and the `DuckDbTableProvider` that exposes the engine's dataframes to DuckDB.
//
// This object is owned by the trace processor (one per `TraceProcessorImpl`)
// and is consulted from `TraceProcessorImpl::ExecuteQuery` before falling back
// to SQLite. It does NOT depend on the rest of the trace_processor `lib`; it
// reaches the live dataframes only through the `Resolver` callback supplied at
// construction (which wraps `PerfettoSqlConnection::GetDataframeOrNull`).
//
// IMPORTANT: driven exclusively through DuckDB's C API so no C++ exception can
// unwind across the boundary into Perfetto's `-fno-exceptions` code.
class DuckDbEngine {
 public:
  using Resolver = DuckDbTableProvider::Resolver;

  // Supplies the PerfettoSQL views to mirror into DuckDB, in creation
  // (dependency) order, as (name, `CREATE VIEW <name> AS <body>` text) pairs.
  // Typically wraps `PerfettoSqlConnection::created_views()`. May be empty.
  using ViewProvider =
      std::function<std::vector<std::pair<std::string, std::string>>()>;

  // A stdlib `RETURNS TABLE` function to mirror into DuckDB as a *table macro*.
  // `name` is the function name, `arg_names` the parameter names in order (no
  // `$` prefix), `body_sql` the SELECT body (with `$arg` bind placeholders).
  struct TableFunction {
    std::string name;
    std::vector<std::string> arg_names;
    std::string body_sql;
  };

  // Supplies the stdlib RETURNS TABLE functions to mirror into DuckDB as table
  // macros, in creation order. Typically wraps
  // `PerfettoSqlConnection::created_table_functions()`. May be empty.
  using FunctionProvider = std::function<std::vector<TableFunction>()>;

  // Supplies the runtime scalar `CREATE PERFETTO FUNCTION`s to mirror into
  // DuckDB as scalar macros (`CREATE MACRO name(args) AS (body)`), in creation
  // order. Reuses the `TableFunction` shape (name, arg_names, body_sql).
  // Typically wraps `PerfettoSqlConnection::created_scalar_functions()`. May be
  // empty.
  using ScalarFunctionProvider = std::function<std::vector<TableFunction>()>;

  // Materializes a plain SQLite-native table (one created with `CREATE TABLE`,
  // e.g. the prelude's `_trace_bounds`) into DuckDB's catalog so a reference to
  // it resolves. Such tables are NOT dataframes/views, so the replacement scan
  // cannot reach them; the engine is deliberately decoupled from SQLite. Given
  // a table name and the DuckDB connection, the callback (which DOES have the
  // SQLite engine) should `CREATE OR REPLACE TABLE <name>` in DuckDB with the
  // table's current rows and return true, or return false if `name` is not a
  // materializable SQLite-native table. Invoked on a DuckDB catalog miss. May
  // be empty (no materialization).
  using TableMaterializer =
      std::function<bool(const std::string& name, duckdb_connection conn)>;

  // `string_pool` and `resolver` must outlive this object. `resolver` is the
  // live read-through into the engine's table registry (typically a lambda
  // calling `PerfettoSqlConnection::GetDataframeOrNull`). `view_provider` (if
  // set) supplies the PerfettoSQL views to mirror into DuckDB's catalog so a
  // bare `FROM <view>` reference (e.g. `sched`, `thread`, `thread_state`)
  // resolves through DuckDB's own view -> the view body's `FROM __intrinsic_*`
  // -> the replacement scan -> `__perfetto_df`.
  DuckDbEngine(StringPool* string_pool,
               Resolver resolver,
               ViewProvider view_provider = {},
               FunctionProvider function_provider = {},
               ScalarFunctionProvider scalar_function_provider = {},
               TableMaterializer table_materializer = {});
  ~DuckDbEngine();

  DuckDbEngine(const DuckDbEngine&) = delete;
  DuckDbEngine& operator=(const DuckDbEngine&) = delete;

  // Provides the per-trace ClockConverter used by the to_monotonic/to_realtime/
  // abs_time_str UDFs. Must be called before the first query (which lazily
  // registers the functions). May be null.
  void SetClockConverter(ClockConverter* converter) {
    clock_converter_ = converter;
  }

  // Provides the arg_set_id -> JSON converter used by the
  // __intrinsic_arg_set_to_json UDF (the args plugin's nested-JSON serializer,
  // reused verbatim). Must be set before the first query. May be empty.
  void SetArgSetJsonConverter(ArgSetJsonConverter converter) {
    arg_set_json_converter_ = std::move(converter);
  }

  // Provides the ftrace-row-id -> systrace-line converter used by the to_ftrace
  // UDF (the to_ftrace plugin's SystraceSerializer, reused verbatim). Must be
  // set before the first query. May be empty.
  void SetToFtraceConverter(Int64ToVarcharConverter converter) {
    to_ftrace_converter_ = std::move(converter);
  }

  // Attempts to run the ENTIRE query `sql` inside DuckDB. Semantics (per D2):
  //  - returns a populated `DuckDbExecutionResult` iff `sql` is a single
  //    relational statement referencing only DuckDB-available relations and
  //    allowlisted functions (the support predicate). The DuckDB query ran and
  //    succeeded; `*ran_in_duckdb` is set true.
  //  - returns `std::nullopt` (no value) iff the query is INELIGIBLE and should
  //    fall back to SQLite. `*ran_in_duckdb` is set false. NOTE: if
  //    `disable_fallback` is true, an ineligible query returns an ERROR instead
  //    (the fallback-honesty gate) so a measurement lane can prove a query
  //    really ran in DuckDB.
  //  - returns an error status iff the query WAS eligible but DuckDB failed to
  //    execute it (a genuine bug/unsupported-construct), or on internal error.
  //    Eligible-but-failed queries surface the error; they do NOT fall back
  //    (that would mask bugs).
  base::StatusOr<std::optional<DuckDbExecutionResult>> TryExecuteWholeQuery(
      const std::string& sql,
      bool disable_fallback,
      bool* ran_in_duckdb);

  // Splits `sql` into (everything up to and including the last top-level `;`,
  // last statement). If `sql` is a single statement, the leading part is empty
  // and the whole `sql` is the last statement. Comments and string/quoted
  // literals are respected so a `;` inside them does not split. This lets the
  // router run the leading PerfettoSQL statements (e.g. INCLUDE PERFETTO
  // MODULE, CREATE PERFETTO TABLE) in the real engine and then try ONLY the
  // final statement inside DuckDB.
  struct SplitStatements {
    std::string leading;  // May be empty (single-statement input).
    std::string last;     // The trailing statement (trimmed of a trailing ';').
  };
  static SplitStatements SplitTrailingStatement(const std::string& sql);

 private:
  // Lazily creates the database/connection + registers the provider on first
  // use. Returns an error if DuckDB initialization fails.
  base::Status EnsureInitialized();

  // Mirrors any PerfettoSQL views from `view_provider_()` that are not yet in
  // DuckDB's catalog. Called before each query so views created after the
  // engine was first initialized (the after_eof prelude, user `INCLUDE`s,
  // runtime CREATE PERFETTO VIEW) become resolvable. Idempotent:
  // already-mirrored views are skipped; a view whose body still cannot bind in
  // DuckDB is left unmirrored (it will be retried on the next query once its
  // dependencies exist).
  void SyncViews();

  // Mirrors any stdlib RETURNS TABLE functions from `function_provider_()` not
  // yet in DuckDB's catalog, as table macros (`CREATE MACRO name(args) AS TABLE
  // <body>`). Called before each query alongside SyncViews. Idempotent: an
  // already-mirrored macro is skipped; a function whose body cannot bind in
  // DuckDB (SQLite-only dialect, or the `__intrinsic_*` table-pointer ABit) is
  // left unmirrored (a query calling it then errors in DuckDB and falls back).
  void SyncTableFunctions();

  // Mirrors any runtime scalar `CREATE PERFETTO FUNCTION`s from
  // scalar_function_provider_() not yet in DuckDB's catalog, as scalar macros
  // (`CREATE MACRO name(args) AS (<body>)`). Called before each query alongside
  // SyncViews. Idempotent; a body DuckDB cannot bind (SQLite-only dialect, an
  // intrinsic, or a recursive self-reference) is left unmirrored. A
  // successfully-mirrored macro's name is added to registered_scalar_functions_
  // so the support predicate treats a call to it as eligible.
  void SyncScalarFunctions();

  // Creates the hardcoded DuckDB table macros that FAITHFULLY emulate
  // Perfetto's C++ slice-tree table intrinsics (ancestor_slice /
  // descendant_slice and their
  // *_and_self variants), replicating the exact C++ algorithms in SQL:
  // ancestor_slice walks slice.parent_id up; descendant_slice uses ts/depth
  // containment plus a parent-chain (recursive) verification for the boundary
  // candidates - matching plugins/{ancestor,descendant}. Created once, after
  // the `slice` view is mirrored, with names added to mirrored_table_macros_.
  void SyncIntrinsicMacros();

  StringPool* string_pool_;
  Resolver resolver_;
  ViewProvider view_provider_;
  FunctionProvider function_provider_;
  ScalarFunctionProvider scalar_function_provider_;
  TableMaterializer table_materializer_;

  // SQLite-native table names already materialized into DuckDB this session (so
  // the catalog-miss retry does not re-materialize on every reference).
  std::unordered_set<std::string> materialized_tables_;

  bool initialized_ = false;

  // The per-trace clock converter for the clock UDFs (may be null). Owned by
  // TraceProcessorContext; set via SetClockConverter before the first query.
  ClockConverter* clock_converter_ = nullptr;

  // arg_set_id -> JSON converter for __intrinsic_arg_set_to_json; set via
  // SetArgSetJsonConverter before the first query. A stable address is passed
  // to the registered UDF, so this member must outlive the connection.
  ArgSetJsonConverter arg_set_json_converter_;
  // ftrace row id -> systrace line for to_ftrace; set via SetToFtraceConverter.
  // Stable address passed to the registered UDF, so it must outlive the conn.
  Int64ToVarcharConverter to_ftrace_converter_;
  duckdb_database db_ = nullptr;
  duckdb_connection conn_ = nullptr;
  std::unique_ptr<DuckDbTableProvider> provider_;

  // Names of PerfettoSQL views successfully mirrored into DuckDB's catalog (so
  // the support predicate treats a reference to one as eligible, delegating the
  // actual binding of the view body to DuckDB).
  std::unordered_set<std::string> mirrored_views_;

  // Lowercased names of stdlib RETURNS TABLE functions successfully mirrored as
  // DuckDB table macros. The support predicate treats a `FROM name(...)` call
  // to one as an eligible table-valued reference (not an unported function).
  std::unordered_set<std::string> mirrored_table_macros_;

  // Lowercased names of runtime scalar functions successfully mirrored as
  // DuckDB scalar macros (tracked to skip re-creating them). Their names are
  // also added to registered_scalar_functions_ so the support predicate allows
  // a call.
  std::unordered_set<std::string> mirrored_scalar_macros_;

  // Set true once any mirrored view/table-macro/scalar-macro BODY references
  // `extract_arg`. extract_arg is most often called INSIDE a view body (e.g.
  // counter_track, journald, gpu_render_stages), not the user's SQL, so the
  // extract_arg index must be built whenever such a view is reachable - not
  // only when the user's query text literally mentions extract_arg.
  bool mirrored_uses_extract_arg_ = false;

  // True once the hardcoded slice-tree intrinsic macros have been created.
  bool intrinsic_macros_created_ = false;

  // Lowercased names of scalar UDFs registered on the DuckDB connection at init
  // (via RegisterScalarFunctions). The support predicate treats a call to one
  // as an eligible function (in addition to the static builtin allowlist).
  std::unordered_set<std::string> registered_scalar_functions_;

  // Backing state for the `extract_arg` UDF: the lazily-built (arg_set_id, key)
  // index over the trace's args table. Owned here so its raw pointer (the UDF's
  // extra_info) outlives the connection. Built on first use of a query that
  // references extract_arg (see EnsureExtractArgIndexBuilt).
  std::unique_ptr<ExtractArgState> extract_arg_state_;
};

// Rewrites a PerfettoSQL `_interval_intersect!((t0, t1, ...), (p0, p1, ...))`
// macro call into an equivalent plain-SQL interval-overlap join, so it runs in
// DuckDB WITHOUT the SQLite-vtable table-pointer machinery the normal macro
// expansion produces (an interval-tree aggregate + __intrinsic_table_ptr). The
// semantics are faithful: the intersection of N intervals is non-empty iff
// greatest(starts) < least(ends), and the partition columns must match across
// all tables. Must be applied BEFORE macro expansion (the macro is otherwise
// expanded to the intrinsic form). Returns the rewritten SQL, or the input
// unchanged if there is no `_interval_intersect!` call to rewrite.
std::string RewriteIntervalIntersectMacro(const std::string& sql);

// Rewrites a PerfettoSQL `_interval_create!(starts, ends)` macro call into
// plain SQL, so it runs in DuckDB without the table-pointer machinery. Faithful
// semantics: for each start ts, the interval duration is (smallest end strictly
// greater than the start) - start; a start with no such end is dropped; output
// ordered by ts. Must be applied BEFORE macro expansion. Returns the input
// unchanged if there is no `_interval_create!` call.
std::string RewriteIntervalCreateMacro(const std::string& sql);

// Rewrites an unqualified `_auto_id` reference to `(row_number() OVER () - 1)`
// (the 0-based dataframe row index the table_provider hides). Conservative:
// only the unqualified form, and the whole input is left unchanged if it
// contains a row-affecting clause (JOIN/WHERE/GROUP/etc.) that would desync
// row_number from the row index. See the definition for the full contract.
std::string RewriteAutoId(const std::string& sql);

// Rewrites the non-partitioned `_interval_intersect_with_col_names!(tab1, id1,
// ts1, dur1, tab2, id2, ts2, dur2, ())` into the native interval_intersect
// combiner form (RegisterIntervalIntersect), reading each table's custom id/
// ts/dur columns. The partitioned form is left to fall back. Must run BEFORE
// macro expansion. Returns the input unchanged if there is no such call.
std::string RewriteIntervalIntersectWithColNamesMacro(const std::string& sql);

// Rewrites every empty double-quoted token `""` to an empty SQL string literal
// `''`. SQLite treats `""` as an empty STRING; DuckDB rejects it at PARSE time
// ("zero-length delimited identifier"), which also prevents the bind-error
// double-quote-literal repair from running on the rest of the statement. `""`
// can never be a real column, so this rewrite is unconditional + safe. Must run
// BEFORE the statement reaches DuckDB.
std::string RewriteEmptyDoubleQuotedString(const std::string& sql);

// Testing-only entry point for the support predicate's TOKENIZATION + decision
// logic, exposed so a unittest can exercise the previously-buggy classification
// cases (CAST(...), USING(...), WITH d(a,b) AS (...), double-quoted literals,
// custom function calls) WITHOUT a live DuckDB connection. Returns the
// ineligible reason (std::nullopt => the query is eligible to route to DuckDB).
// The two sets are the function-name allowlist and the registered-UDF names the
// real engine would consult; the analysis is otherwise a pure function of
// `sql`.
namespace internal {
std::optional<std::string> AnalyzeSupportForTesting(
    const std::string& sql,
    const std::unordered_set<std::string>& builtin_allowlist,
    const std::unordered_set<std::string>& registered_udfs,
    const std::unordered_set<std::string>& table_macros = {});
}  // namespace internal

}  // namespace perfetto::trace_processor::duckdb_integration

#endif  // SRC_TRACE_PROCESSOR_DUCKDB_DUCKDB_ENGINE_H_
