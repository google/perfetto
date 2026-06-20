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

#ifndef SRC_TRACE_PROCESSOR_DUCKDB_CLOCK_FUNCTION_H_
#define SRC_TRACE_PROCESSOR_DUCKDB_CLOCK_FUNCTION_H_

#include "duckdb.h"

#include "perfetto/base/status.h"

namespace perfetto::trace_processor {

class ClockConverter;

namespace duckdb_integration {

// Registers DuckDB scalar UDFs for the clock-conversion functions that
// PerfettoSQL exposes (and delegates to C++ intrinsics): `to_monotonic(ts)` and
// `to_realtime(ts)` (BIGINT -> BIGINT) and `abs_time_str(ts)` (BIGINT ->
// VARCHAR). Each calls the per-trace `ClockConverter`; a conversion error or a
// NULL input yields NULL (matching the SQLite intrinsics). `converter` may be
// null (e.g. no trace loaded), in which case the UDFs return NULL.
base::Status RegisterClockFunctions(duckdb_connection conn,
                                    ClockConverter* converter);

}  // namespace duckdb_integration
}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_DUCKDB_CLOCK_FUNCTION_H_
