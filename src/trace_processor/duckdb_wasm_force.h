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

#ifndef SRC_TRACE_PROCESSOR_DUCKDB_WASM_FORCE_H_
#define SRC_TRACE_PROCESSOR_DUCKDB_WASM_FORCE_H_

#include <cstdint>

namespace perfetto {
namespace trace_processor {

// EXPERIMENTAL (M2 size gate only). Runs `SELECT 42` inside an in-memory DuckDB
// and returns the result (42), or -1 on error. Used only to keep DuckDB from
// being dead-code-stripped out of the Wasm bundle so we can measure its size.
int64_t DuckDbForceLinkSmoke();

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DUCKDB_WASM_FORCE_H_
