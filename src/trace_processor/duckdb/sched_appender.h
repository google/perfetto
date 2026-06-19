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

#ifndef SRC_TRACE_PROCESSOR_DUCKDB_SCHED_APPENDER_H_
#define SRC_TRACE_PROCESSOR_DUCKDB_SCHED_APPENDER_H_

#include "duckdb.h"

#include "perfetto/base/status.h"
#include "src/trace_processor/core/dataframe/dataframe.h"

namespace perfetto::trace_processor::duckdb_integration {

// M3a (experimental DuckDB query engine): materializes a `sched`-schema
// `dataframe::Dataframe` into a native DuckDB table named `sched` via the
// DuckDB *C API* (Appender + data chunks). This is the "simplest thing that
// works" path: it copies the whole dataframe so DuckDB owns the storage and
// can run a real `SELECT ... FROM sched WHERE ...` entirely inside DuckDB.
//
// The expected columns (in order) match `tables/sched_tables.py`:
//   id UINTEGER, ts BIGINT, dur BIGINT, utid UINTEGER, end_state VARCHAR,
//   priority INTEGER, ucpu UINTEGER
// where `id` is synthesised from the row offset and `end_state` is the only
// nullable/string column (resolved through the dataframe's StringPool).
//
// IMPORTANT: this is driven exclusively through DuckDB's C API so no C++
// exception can ever unwind across the boundary into Perfetto's
// `-fno-exceptions` code (see the DuckDB integration contract). Errors are
// reported as a `base::Status`.
//
// `connection` must be an open DuckDB connection. On success the `sched` table
// exists and is populated.
base::Status AppendSchedDataframe(duckdb_connection connection,
                                  const dataframe::Dataframe& sched);

}  // namespace perfetto::trace_processor::duckdb_integration

#endif  // SRC_TRACE_PROCESSOR_DUCKDB_SCHED_APPENDER_H_
