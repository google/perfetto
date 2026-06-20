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

#ifndef SRC_TRACE_PROCESSOR_DUCKDB_GRAPH_FUNCTION_H_
#define SRC_TRACE_PROCESSOR_DUCKDB_GRAPH_FUNCTION_H_

#include "duckdb.h"

#include "perfetto/base/status.h"

namespace perfetto::trace_processor::duckdb_integration {

// Registers the DuckDB functions implementing `graph_reachable_bfs!` and
// `graph_reachable_dfs!` by reusing Trace Processor's native BFS/DFS reachable
// traversal (graph_traversal plugin), via the aggregate + combiner pattern
// (see RegisterIntervalIntersect for the rationale - DuckDB table functions
// reject non-constant args, but aggregates/scalars do not):
//
//   __intrinsic_graph_agg(source_node_id BIGINT, dest_node_id BIGINT) -> BIGINT
//       Aggregate. Collects directed edges into a buffer; returns a handle.
//   __intrinsic_int_array_agg(node_id BIGINT) -> BIGINT
//       Aggregate. Collects the start-node ids into a buffer; returns a handle.
//   __intrinsic_graph_bfs(graph BIGINT, starts BIGINT)
//   __intrinsic_graph_dfs(graph BIGINT, starts BIGINT)
//       Scalars. Read the two buffers, run BFS/DFS reachability, and return the
//       result as LIST<STRUCT(node_id, parent_node_id)> (the rewrite UNNESTs).
base::Status RegisterGraphFunctions(duckdb_connection conn);

}  // namespace perfetto::trace_processor::duckdb_integration

#endif  // SRC_TRACE_PROCESSOR_DUCKDB_GRAPH_FUNCTION_H_
