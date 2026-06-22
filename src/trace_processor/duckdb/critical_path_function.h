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

#ifndef SRC_TRACE_PROCESSOR_DUCKDB_CRITICAL_PATH_FUNCTION_H_
#define SRC_TRACE_PROCESSOR_DUCKDB_CRITICAL_PATH_FUNCTION_H_

#include "duckdb.h"

#include "perfetto/base/status.h"

namespace perfetto::trace_processor::duckdb_integration {

// Registers the critical-path-walk intrinsics (a verbatim port of the
// critical_path plugin):
//   * __intrinsic_wakeup_graph_agg(id, utid, ts, dur, idle_dur, waker_id,
//     prev_id) -> BIGINT handle to a WakeupGraph.
//   * __intrinsic_cp_roots_agg(root_id) -> BIGINT handle to the root id list.
//   * __intrinsic_critical_path_walk(graph_handle, roots_handle) ->
//     LIST<STRUCT(root_id, depth, ts, dur, blocker_id, blocker_utid,
//     parent_id)>.
// Emitted only by the _critical_path_with_depth_by_roots! macro override.
base::Status RegisterCriticalPath(duckdb_connection conn);

}  // namespace perfetto::trace_processor::duckdb_integration

#endif  // SRC_TRACE_PROCESSOR_DUCKDB_CRITICAL_PATH_FUNCTION_H_
