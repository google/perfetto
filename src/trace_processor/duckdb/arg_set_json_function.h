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

#ifndef SRC_TRACE_PROCESSOR_DUCKDB_ARG_SET_JSON_FUNCTION_H_
#define SRC_TRACE_PROCESSOR_DUCKDB_ARG_SET_JSON_FUNCTION_H_

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "duckdb.h"

#include "perfetto/base/status.h"

namespace perfetto::trace_processor::duckdb_integration {

// A converter from arg_set_id to its JSON-object string (the shared C++ core of
// the SQLite `__intrinsic_arg_set_to_json` function, exposed by the args plugin
// so the DuckDB lane reuses the EXACT same nested-JSON serialization). Returns
// nullopt on an internal error (-> NULL). Implementations need not be
// thread-safe; the closure passed in serializes if its backing state requires.
using ArgSetJsonConverter = std::function<std::optional<std::string>(int64_t)>;

// Registers the DuckDB scalar `__intrinsic_arg_set_to_json(BIGINT) -> VARCHAR`,
// delegating to `*converter` (a stable pointer; may hold an empty std::function
// when no trace/converter is wired, in which case calls return NULL). A NULL
// input or a converter error yields NULL (matching the SQLite intrinsic).
base::Status RegisterArgSetJsonFunction(duckdb_connection conn,
                                        const ArgSetJsonConverter* converter);

}  // namespace perfetto::trace_processor::duckdb_integration

#endif  // SRC_TRACE_PROCESSOR_DUCKDB_ARG_SET_JSON_FUNCTION_H_
