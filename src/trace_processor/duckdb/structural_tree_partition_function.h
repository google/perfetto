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

#ifndef SRC_TRACE_PROCESSOR_DUCKDB_STRUCTURAL_TREE_PARTITION_FUNCTION_H_
#define SRC_TRACE_PROCESSOR_DUCKDB_STRUCTURAL_TREE_PARTITION_FUNCTION_H_

#include "duckdb.h"

#include "perfetto/base/status.h"

namespace perfetto::trace_processor::duckdb_integration {

// Registers `__intrinsic_structural_tree_partition(id, parent_id, group_key)`,
// an aggregate that returns LIST<STRUCT(node_id, parent_node_id, group_key)>:
// the structural partition of the input tree (each node's nearest same-group
// ancestor), a verbatim port of the structural_tree_partition plugin. Emitted
// only by the `tree_structural_partition_by_group!` macro override.
base::Status RegisterStructuralTreePartition(duckdb_connection conn);

}  // namespace perfetto::trace_processor::duckdb_integration

#endif  // SRC_TRACE_PROCESSOR_DUCKDB_STRUCTURAL_TREE_PARTITION_FUNCTION_H_
