// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Shared declarations of the perfetto DuckDB extension. Every module includes
// this header; only perfetto_ext.cc defines the C API global.
#ifndef PERFETTO_BIGTRACE_DUCKDB_EXT_EXTENSION_H_
#define PERFETTO_BIGTRACE_DUCKDB_EXT_EXTENSION_H_

#define DUCKDB_EXTENSION_NAME perfetto
#include "duckdb_extension.h"  // no-include-violation-check

DUCKDB_EXTENSION_EXTERN

// Registers pf_compile_pipe (pipe.cc). Returns false on failure.
bool RegisterPipeFunctions(duckdb_connection conn);

// Register the native kernels of the interval, tree and graph operators
// (intervals.cc, trees.cc, graphs.cc) and their SQL generators (sql_gen.cc).
// The SQL macros of sql/*.sql, run after these, build on them. Return false
// on failure.
bool RegisterIntervalFunctions(duckdb_connection conn);
bool RegisterTreeFunctions(duckdb_connection conn);
bool RegisterGraphFunctions(duckdb_connection conn);
bool RegisterSqlGenFunctions(duckdb_connection conn);

#endif  // PERFETTO_BIGTRACE_DUCKDB_EXT_EXTENSION_H_
