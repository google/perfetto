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

#ifndef SRC_TRACE_PROCESSOR_DUCKDB_EXTRACT_ARG_H_
#define SRC_TRACE_PROCESSOR_DUCKDB_EXTRACT_ARG_H_

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "duckdb.h"

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"

namespace perfetto::trace_processor::duckdb_integration {

// Lazily-built index of the trace's `args` table, keyed by (arg_set_id, key),
// backing the `extract_arg` UDF. Owned by the DuckDbEngine (its raw pointer is
// the UDF's extra_info, so it must outlive the connection). The index is built
// once, on first use, by querying `__intrinsic_args` through the engine's own
// DuckDB connection (so it transparently reuses the replacement scan).
struct ExtractArgState {
  // The polymorphic value an arg holds, mirroring SQLite's `extract_arg` which
  // returns the arg's NATURAL type (an int arg surfaces as an integer, a string
  // arg as text, etc.). Reproduced in DuckDB via a UNION(i, r, s) return type.
  struct Value {
    enum class Kind { kInt, kReal, kString } kind;
    int64_t i = 0;
    double r = 0;
    std::string s;
  };

  bool built = false;
  std::unordered_map<int64_t, std::unordered_map<std::string, Value>> index;
};

// Registers `extract_arg(BIGINT, VARCHAR)` (and its `__intrinsic_extract_arg`
// alias) on `conn`, returning the owned state object the UDF reads through.
// `extract_arg` is added to `out_registered` so the support predicate treats it
// as an eligible function. The returned state must outlive `conn`.
base::StatusOr<std::unique_ptr<ExtractArgState>> RegisterExtractArg(
    duckdb_connection conn,
    std::unordered_set<std::string>* out_registered);

// Ensures `state`'s (arg_set_id, key) index is built, querying `__intrinsic_args`
// via `conn` on first call. Idempotent. Must be called (outside the UDF
// trampoline) before a query that invokes `extract_arg` runs, because building
// the index issues its own DuckDB query and must not re-enter execution.
base::Status EnsureExtractArgIndexBuilt(duckdb_connection conn,
                                        ExtractArgState* state);

}  // namespace perfetto::trace_processor::duckdb_integration

#endif  // SRC_TRACE_PROCESSOR_DUCKDB_EXTRACT_ARG_H_
