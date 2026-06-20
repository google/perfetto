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

#ifndef SRC_TRACE_PROCESSOR_DUCKDB_INTERVAL_INTERSECT_FUNCTION_H_
#define SRC_TRACE_PROCESSOR_DUCKDB_INTERVAL_INTERSECT_FUNCTION_H_

#include "duckdb.h"

#include "perfetto/base/status.h"

namespace perfetto::trace_processor::duckdb_integration {

// Registers the DuckDB functions that implement `_interval_intersect!` by
// reusing Trace Processor's native N-way interval-intersection algorithm
// (src/trace_processor/containers/interval_intersector.h) instead of desugaring
// to plain SQL (which the planner can turn into an O(N^k) blow-up).
//
// Two functions are registered, mirroring the SQLite-vtable design (aggregate
// the rows of each table into an opaque handle, then a combiner runs the
// algorithm over the handles):
//
//   __intrinsic_ii_agg(id BIGINT, ts BIGINT, dur BIGINT) -> BIGINT
//       Aggregate. Collects a table's intervals into a heap buffer and returns
//       an opaque handle (an index into a process-global registry).
//
//   __intrinsic_ii_combine(LIST<BIGINT> handles)
//       -> LIST<STRUCT(ts, dur, id_0 .. id_14)>
//       Scalar. Reads the buffers for the given handles, runs the N-way
//       intersection, frees the buffers, and returns the result rows as a list
//       of structs (so the caller `UNNEST`s them into rows). 15 id columns is
//       the existing per-call table limit, so the struct width is fixed and no
//       dynamic bind is needed; the rewrite selects only id_0..id_{n-1}.
base::Status RegisterIntervalIntersect(duckdb_connection conn);

}  // namespace perfetto::trace_processor::duckdb_integration

#endif  // SRC_TRACE_PROCESSOR_DUCKDB_INTERVAL_INTERSECT_FUNCTION_H_
