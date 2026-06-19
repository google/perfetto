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

#ifndef SRC_TRACE_PROCESSOR_DUCKDB_SCALAR_FUNCTIONS_H_
#define SRC_TRACE_PROCESSOR_DUCKDB_SCALAR_FUNCTIONS_H_

#include <string>
#include <unordered_set>

#include "duckdb.h"

#include "perfetto/base/status.h"

namespace perfetto::trace_processor::duckdb_integration {

// Registers the first batch of pure (context-free) PerfettoSQL scalar UDFs on a
// DuckDB connection, using DuckDB's C scalar-function API. This is the REUSABLE
// registration mechanism the broader UDF-porting tier reuses: each function is
// registered via a small `RegisterScalarFunction` helper (see the .cc) that
// wires a vectorized trampoline + per-arg logical types + return type.
//
// The functions registered here mirror the EXACT semantics of the corresponding
// Perfetto `__intrinsic_*` C++ implementations (in src/trace_processor/plugins),
// NOT DuckDB's native builtins (whose semantics diverge: e.g. DuckDB `unhex`
// returns BLOB not an integer, DuckDB `regexp_extract` returns the whole match
// and "" on no-match rather than group 1 / NULL).
//
// Each function is registered under BOTH its public PerfettoSQL surface name
// (e.g. `ln`, `regexp_extract`, `unhex`) AND its underlying `__intrinsic_*`
// name. The surface name is what binds when a user query reaches DuckDB (the raw
// user SQL uses surface names; the `DELEGATES TO __intrinsic_*` rewrite happens
// inside the SQLite-backed engine, not before DuckDB sees the query). Both names
// are returned in `out_registered` so the support predicate can allowlist them.
// DuckDB registers user overloads with ALTER_ON_CONFLICT, so overriding a native
// builtin overload of the same signature is allowed.
//
// C API ONLY: no `duckdb::` type and no C++ exception crosses the boundary.
//
// `out_registered` is populated with the lowercased names successfully
// registered (so the caller can add them to the function allowlist).
base::Status RegisterScalarFunctions(
    duckdb_connection conn,
    std::unordered_set<std::string>* out_registered);

}  // namespace perfetto::trace_processor::duckdb_integration

#endif  // SRC_TRACE_PROCESSOR_DUCKDB_SCALAR_FUNCTIONS_H_
