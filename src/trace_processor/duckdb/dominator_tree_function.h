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

#ifndef SRC_TRACE_PROCESSOR_DUCKDB_DOMINATOR_TREE_FUNCTION_H_
#define SRC_TRACE_PROCESSOR_DUCKDB_DOMINATOR_TREE_FUNCTION_H_

#include "duckdb.h"

#include "perfetto/base/status.h"

namespace perfetto::trace_processor::duckdb_integration {

// Registers the DuckDB aggregate implementing `graph_dominator_tree!` by
// reusing Trace Processor's native Lengauer-Tarjan dominator-tree algorithm
// (dominator_tree plugin). Because the dominator tree takes a SINGLE input
// relation (the graph; the root is a scalar arg), the aggregate can return the
// result rows directly as a LIST<STRUCT> (no separate combiner/handle needed):
//
//   __intrinsic_dominator_tree(source_node_id BIGINT, dest_node_id BIGINT,
//                              root_node_id BIGINT)
//       -> LIST<STRUCT(node_id, dominator_node_id)>
//
// The rewrite UNNESTs it: `SELECT unnest(__intrinsic_dominator_tree(...)) FROM
// <graph_table>`.
base::Status RegisterDominatorTree(duckdb_connection conn);

}  // namespace perfetto::trace_processor::duckdb_integration

#endif  // SRC_TRACE_PROCESSOR_DUCKDB_DOMINATOR_TREE_FUNCTION_H_
