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

// A converter from a BIGINT key (arg_set_id, ftrace row id, ...) to a string,
// reusing the EXACT C++ logic of the matching SQLite intrinsic so the DuckDB
// lane is byte-identical. Returns nullopt on an internal error /
// un-serializable input (-> NULL). Need not be thread-safe; the closure
// serializes if its backing state (mutable scratch) requires.
using Int64ToVarcharConverter =
    std::function<std::optional<std::string>(int64_t)>;
using ArgSetJsonConverter = Int64ToVarcharConverter;  // back-compat alias.

// Registers a DuckDB scalar `name(BIGINT) -> VARCHAR` delegating to
// `*converter` (a stable pointer; may hold an empty std::function when unwired
// -> NULL). A NULL input or a converter error yields NULL.
base::Status RegisterInt64ToVarcharFunction(
    duckdb_connection conn,
    const char* name,
    const Int64ToVarcharConverter* converter);

// Registers `__intrinsic_arg_set_to_json(BIGINT) -> VARCHAR` (the args plugin's
// nested-JSON serializer). Thin wrapper over RegisterInt64ToVarcharFunction.
base::Status RegisterArgSetJsonFunction(duckdb_connection conn,
                                        const ArgSetJsonConverter* converter);

}  // namespace perfetto::trace_processor::duckdb_integration

#endif  // SRC_TRACE_PROCESSOR_DUCKDB_ARG_SET_JSON_FUNCTION_H_
