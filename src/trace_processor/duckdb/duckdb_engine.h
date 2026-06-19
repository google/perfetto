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
#include <memory>
#include <optional>
#include <string>

#include "duckdb.h"

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/duckdb/duckdb_iterator_impl.h"
#include "src/trace_processor/duckdb/table_provider.h"

namespace perfetto::trace_processor {
class StringPool;
}  // namespace perfetto::trace_processor

namespace perfetto::trace_processor::duckdb_integration {

// The engine-facing entry point for the experimental DuckDB query engine (the
// SQLite -> DuckDB migration). Owns a lazily-created DuckDB database/connection
// and the `DuckDbTableProvider` that exposes the engine's dataframes to DuckDB.
//
// This object is owned by the trace processor (one per `TraceProcessorImpl`) and
// is consulted from `TraceProcessorImpl::ExecuteQuery` before falling back to
// SQLite. It does NOT depend on the rest of the trace_processor `lib`; it
// reaches the live dataframes only through the `Resolver` callback supplied at
// construction (which wraps `PerfettoSqlConnection::GetDataframeOrNull`).
//
// IMPORTANT: driven exclusively through DuckDB's C API so no C++ exception can
// unwind across the boundary into Perfetto's `-fno-exceptions` code.
class DuckDbEngine {
 public:
  using Resolver = DuckDbTableProvider::Resolver;

  // `string_pool` and `resolver` must outlive this object. `resolver` is the
  // live read-through into the engine's table registry (typically a lambda
  // calling `PerfettoSqlConnection::GetDataframeOrNull`).
  DuckDbEngine(StringPool* string_pool, Resolver resolver);
  ~DuckDbEngine();

  DuckDbEngine(const DuckDbEngine&) = delete;
  DuckDbEngine& operator=(const DuckDbEngine&) = delete;

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

 private:
  // Lazily creates the database/connection + registers the provider on first
  // use. Returns an error if DuckDB initialization fails.
  base::Status EnsureInitialized();

  StringPool* string_pool_;
  Resolver resolver_;

  bool initialized_ = false;
  duckdb_database db_ = nullptr;
  duckdb_connection conn_ = nullptr;
  std::unique_ptr<DuckDbTableProvider> provider_;
};

}  // namespace perfetto::trace_processor::duckdb_integration

#endif  // SRC_TRACE_PROCESSOR_DUCKDB_DUCKDB_ENGINE_H_
