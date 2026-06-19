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

#ifndef SRC_TRACE_PROCESSOR_DUCKDB_SCHED_TABLE_FUNCTION_H_
#define SRC_TRACE_PROCESSOR_DUCKDB_SCHED_TABLE_FUNCTION_H_

#include "duckdb.h"

#include "perfetto/base/status.h"
#include "src/trace_processor/core/dataframe/dataframe.h"

namespace perfetto::trace_processor::duckdb_integration {

// M3b (experimental DuckDB query engine): registers a DuckDB *table function*
// named `sched_df` that lets DuckDB scan a live `sched`-schema
// `dataframe::Dataframe` directly. Unlike M3a's appender (which COPIES the whole
// dataframe into a native DuckDB table), DuckDB *owns the scan* here: it pulls
// chunks lazily from the dataframe through the table-function callbacks. This is
// the "real target" of M3 - no second materialised table.
//
// Usage from SQL once registered:
//   SELECT count(*), sum(dur) FROM sched_df() WHERE ucpu = 3;
//
// The result columns (in order) match `tables/sched_tables.py`:
//   id UINTEGER, ts BIGINT, dur BIGINT, utid UINTEGER, end_state VARCHAR,
//   priority INTEGER, ucpu UINTEGER
// where `id` is synthesised from the row offset and `end_state` is the only
// nullable/string column (resolved through the dataframe's StringPool).
//
// Lifetime: a `CopyFinalized()` snapshot of `sched` is taken at registration
// time and stored as the table function's extra-info. DuckDB pulls chunks
// lazily (and, in principle, across threads), so the snapshot must outlive every
// query that scans `sched_df`. The snapshot is a shallow shared-ptr copy of the
// column buffers, so it is cheap and keeps the underlying storage alive. It is
// released by the extra-info destructor when the table function is destroyed
// (i.e. when its owning connection/database is torn down).
//
// Capabilities / limitations (DuckDB C table-function API):
//   - Projection pushdown IS supported (only requested columns are filled).
//   - Filter pushdown is NOT exposed by the C API; DuckDB filters post-scan.
//
// IMPORTANT: driven exclusively through DuckDB's C API so no C++ exception can
// unwind across the boundary into Perfetto's `-fno-exceptions` code.
//
// `connection` must be an open DuckDB connection. On success a `sched_df` table
// function is registered on that connection's database.
base::Status RegisterSchedTableFunction(duckdb_connection connection,
                                        const dataframe::Dataframe& sched);

}  // namespace perfetto::trace_processor::duckdb_integration

#endif  // SRC_TRACE_PROCESSOR_DUCKDB_SCHED_TABLE_FUNCTION_H_
